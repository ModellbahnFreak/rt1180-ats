package main

import (
	"flag"
	"fmt"
	"go-ats/packet"
	"log"
	"strconv"
	"strings"
	"time"
)

type streamSpec struct {
	cbs float64
	cir float64
}

var (
	in      string        = "timings.db"
	streams []*streamSpec = make([]*streamSpec, 0)
)

func init() {
	flag.StringVar(&in, "in", in, "Input file")
	flag.Func("stream", "stream parameters for the nth stream (order matters) in the form cbs,cir (e.g. 1500,125000000)", func(s string) error {
		parts := strings.Split(s, ",")
		if len(parts) != 2 {
			return fmt.Errorf("invaid stream specification for stream %d (expecting two numbers separated by ,): %s", len(streams), s)
		}
		cbs, err := strconv.ParseFloat(strings.TrimSpace(parts[0]), 64)
		if err != nil {
			return fmt.Errorf("invaid stream specification for stream %d (error parsing cbs: %v): %s", len(streams), err, s)
		}
		cir, err := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64)
		if err != nil {
			return fmt.Errorf("invaid stream specification for stream %d (error parsing cir: %v): %s", len(streams), err, s)
		}
		streams = append(streams, &streamSpec{
			cbs: cbs, cir: cir,
		})
		return nil
	})
}

func main() {

	flag.Parse()

	var reader packet.DataInput
	if strings.HasSuffix(strings.ToLower(in), ".db") {
		out := packet.DataOutput_Sqlite(in)
		out.Close()
		reader = packet.DataInput_Sqlite(in, "packets,stream", "", "", false, 1, -400)
	} else {
		reader = packet.DataInput_Csv(in, "packets,stream", "", "", false, 1, -400)
	}

	log.Println("Reading all packets")
	packets := reader.ReadAllPackets()

	log.Println("Starting eligibility time verificaion")

	groupEt := make(map[uint8]time.Duration, len(streams))
	bucketEmpty := make(map[uint8]time.Duration, len(streams))
	numPacketsInStream := make(map[uint8]int, len(streams))
	lastPacket := make(map[uint8]*packet.PacketTimings)
	for i := range packets {
		p := packets[i]
		if p == nil {
			continue
		}
		if len(streams) < int(p.Stream) {
			log.Fatalf("Did not know stream %d, specify stream params using -stream", p.Stream)
		}
		if p.Eligibility < p.Enqueue {
			log.Fatalf("Packet %d/%d (%d) eligible at %d before its arrival at %d", p.Stream, p.I, p.Eligibility, p.Enqueue, numPacketsInStream[p.Stream])
		}

		l := p.Size
		if l == 0 {
			l = 1500
		}
		lengthRecoveryDuration := time.Duration(float64(time.Second) * float64(l) / streams[p.Stream].cir)
		emptyToFullDuration := time.Duration(float64(time.Second) * streams[p.Stream].cbs / streams[p.Stream].cir)

		if _, has := groupEt[p.Stream]; !has {
			groupEt[p.Stream] = p.Eligibility
			if p.Eligibility > p.Enqueue {
				log.Printf("Packet %d/%d (%d): Initialized emtpy time from delayed packet", p.Stream, p.I, numPacketsInStream[p.Stream])
				bucketEmpty[p.Stream] = p.Eligibility
			}
			numPacketsInStream[p.Stream]++
			continue
		}
		if _, has := bucketEmpty[p.Stream]; !has {
			d := p.Eligibility - groupEt[p.Stream]
			if d >= emptyToFullDuration {
				bucketEmpty[p.Stream] = p.Eligibility - emptyToFullDuration + lengthRecoveryDuration
				log.Printf("Packet %d/%d (%d): Initialized emtpy time from long interval", p.Stream, p.I, numPacketsInStream[p.Stream])
			}
			groupEt[p.Stream] = p.Eligibility
			numPacketsInStream[p.Stream]++
			continue
		}

		schedulerEligibilityTime := bucketEmpty[p.Stream] + lengthRecoveryDuration
		bucketFullTime := bucketEmpty[p.Stream] + emptyToFullDuration
		eligibilityTime := max(p.Enqueue, groupEt[p.Stream], schedulerEligibilityTime)
		groupEt[p.Stream] = eligibilityTime
		if eligibilityTime < bucketFullTime {
			bucketEmpty[p.Stream] = schedulerEligibilityTime
		} else {
			bucketEmpty[p.Stream] = schedulerEligibilityTime + eligibilityTime - bucketFullTime
		}
		if p.Eligibility != eligibilityTime {
			if lastP, has := lastPacket[p.Stream]; has {
				d := p.Eligibility - lastP.Eligibility
				numMissing := d / lengthRecoveryDuration
				if d > 2*emptyToFullDuration && lastP.I+uint64(numMissing) == p.I {
					log.Printf("EDGE CASE: Missing %d packets (%d/%d through %d/%d) that were correctly scheduled", numMissing, p.Stream, lastP.I+1, p.Stream, p.I-1)
					if p.Eligibility > p.Enqueue {
						bucketEmpty[p.Stream] = p.Eligibility
						groupEt[p.Stream] = p.Eligibility
					} else {
						delete(bucketEmpty, p.Stream)
						delete(groupEt, p.Stream)
					}
				} else {
					log.Printf("Packet %d/%d (%d) had eligibility time %d, expected %d, offset %d; %d", p.Stream, p.I, numPacketsInStream[p.Stream], p.Eligibility, eligibilityTime, p.Eligibility-eligibilityTime, numMissing)
					delete(bucketEmpty, p.Stream)
					delete(groupEt, p.Stream)
				}
			}
		}
		lastPacket[p.Stream] = p

		numPacketsInStream[p.Stream]++
	}

	log.Println("All eligibility times correct")
}
