package main

import (
	"compress/gzip"
	"flag"
	"fmt"
	"go-ats/packet"
	"io"
	"io/fs"
	"log"
	"os"
	"path"
	"slices"
	"strings"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/pcapgo"
)

var (
	inFile             string = ""
	folderPath         string
	ignoreInitialFiles bool   = true
	waitForNextFile    bool   = true
	calculateSpeed     int    = 0
	outFile            string = "timings.csv"
	stopAfter          int    = 0
)

type CapReader interface {
	ZeroCopyReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error)
}

const SPEED_EXCLUSIVE = false // set true to only produce speed result if -speed is set

var knownFileSizes map[string]int64 = make(map[string]int64)
var speed *speed_analyzer
var totalPacketCount = 0
var ERR_STOP_LIMIT_REACHED = fmt.Errorf("Reached set packet limit")

func init() {
	flag.StringVar(&inFile, "in", inFile, "Input pcap(ng) file to parse. If set, no automatic file detection will happen")
	flag.StringVar(&folderPath, "folder", folderPath, "Path to the folder of pcap files to parse")
	flag.BoolVar(&ignoreInitialFiles, "ignore", ignoreInitialFiles, "Set this flag, to ognore all files initially in the dir")
	flag.BoolVar(&waitForNextFile, "nextFile", waitForNextFile, "Set this flag, to only read files once the next file exists")
	flag.IntVar(&calculateSpeed, "speed", calculateSpeed, "Set this flag to the bucket size in ms to calculate the average speed at which packets are received. Exported to separate csv.")
	flag.StringVar(&outFile, "out", outFile, "Path to the output csv file")
	flag.IntVar(&stopAfter, "stopAfter", stopAfter, "Number of packets to stop parsing after (<= 0 no limit)")
}

func main() {
	flag.Parse()

	folderPath = path.Clean(folderPath)

	if len(inFile) == 0 && ignoreInitialFiles {
		files := findAllPcapFiles(true)
		log.Printf("Ignoring initial files: %v", files)
		for f := range knownFileSizes {
			knownFileSizes[f] = -1
		}
	}

	var out packet.DataOutput
	if strings.HasSuffix(outFile, ".db") {
		out = packet.DataOutput_Sqlite(outFile)
	} else {
		out = packet.DataOutput_Csv(outFile)
	}
	defer out.Close()
	if sqlO, is := out.(*packet.SqliteOutput); is {
		defer func() {
			sqlO.Flush()
			sqlO.AddNumbers()
		}()
	}

	defer func() {
		packets := flushPackets(-1)
		for _, p := range packets {
			out.WritePacket(p)
		}
	}()

	if calculateSpeed > 0 {
		speed = speed_analyzer_new(out, calculateSpeed)
		defer speed.speed_analyzer_close()
	}

	if len(inFile) > 0 {
		processPcapFile(inFile, out)
	} else {
		t := time.NewTicker(1 * time.Second)
		for range t.C {
			log.Println("Reading folder")
			files := findAllPcapFiles(false)
			for _, f := range files {
				err := processPcapFile(path.Join(folderPath, f), out)
				if err != nil {
					if err == ERR_STOP_LIMIT_REACHED {
						break
					} else {
						log.Fatalln("error in pcap processing:", err)
					}
				}
				knownFileSizes[f] = -1
			}
			if !waitForNextFile && !ignoreInitialFiles {
				return
			}
		}
	}
}

func processPcapFile(f string, out packet.DataOutput) error {
	log.Printf("Processing %s", f)
	var reader io.Reader
	fRead, err := os.Open(f)
	if err != nil {
		return fmt.Errorf("could not read file %s: %v", f, err)
	}
	defer fRead.Close()
	var has bool
	if f, has = strings.CutSuffix(strings.ToLower(f), ".gz"); has {
		gzRead, err := gzip.NewReader(fRead)
		if err != nil {
			return fmt.Errorf("Could not ungzip compressed file %s.gz: %v", f, err)
		}
		defer gzRead.Close()
		reader = gzRead
	} else {
		reader = fRead
	}

	var capRead CapReader
	if strings.HasSuffix(f, ".pcapng") {
		capRead, err = pcapgo.NewNgReader(reader, pcapgo.DefaultNgReaderOptions)
		if err != nil {
			return fmt.Errorf("Could not read pcapng file %s: %v", f, err)
		}
	} else {
		capRead, err = pcapgo.NewReader(reader)
		if err != nil {
			return fmt.Errorf("Could not read pcap file %s: %v", f, err)
		}
	}

	defer out.Flush()
	defer packetMissingReport()
	for i := 0; ; i++ {
		data, info, err := capRead.ZeroCopyReadPacketData()
		if err != nil {
			log.Printf("Processed %d packets from %s", i, f)
			if err != io.EOF {
				log.Printf("Finishing reading %s due to %v", f, err)
			}
			return nil
		}
		if !SPEED_EXCLUSIVE || speed == nil {
			packets := parsePacket(i, data, info)
			for _, p := range packets {
				out.WritePacket(p)
			}
		}
		if speed != nil {
			speed.speed_analyzer_addPacket(i, data, info)
		}
		totalPacketCount++
		if totalPacketCount >= stopAfter {
			return ERR_STOP_LIMIT_REACHED
		}
	}
}

// 0: no, 0b001: yes, 0b010: gz, 0b100: pcapng
func isPcapFile(f string) (res int) {
	nameLower := strings.ToLower(f)
	var has bool
	if nameLower, has = strings.CutSuffix(nameLower, ".gz"); has {
		res |= 0b010
	}
	if strings.HasSuffix(nameLower, ".pcap") {
		res |= 0b001
	} else if strings.HasSuffix(nameLower, ".pcapng") {
		res |= 0b101
	} else {
		return 0
	}
	return
}

func findAllPcapFiles(ignore bool) []string {
	folder, err := os.ReadDir(folderPath)
	if err != nil {
		log.Fatalln("Could not read files from folder ", folderPath, " ", err)
	}
	files := make([]string, 0)
	slices.SortFunc(folder, func(f1, f2 os.DirEntry) int { return strings.Compare(f1.Name(), f2.Name()) })
	for i := range folder {
		if isPcapFile(folder[i].Name()) == 0 {
			continue
		}

		var info fs.FileInfo = nil
		var oldFileSize int64
		var has bool
		if oldFileSize, has = knownFileSizes[folder[i].Name()]; has {
			if oldFileSize == -1 { // ignore any file which has -1 size set (ignore override)
				continue
			}

			info, err = folder[i].Info()
			if err != nil {
				log.Fatalf("Could not read file info of %s: %v", folder[i].Name(), err)
			}
			if info.Size() > oldFileSize {
				log.Printf("File %s still growing at %d bytes. Ignoring", info.Name(), info.Size())
				knownFileSizes[info.Name()] = info.Size()
				continue
			} else if info.Size() < oldFileSize {
				log.Fatalf("File %s shrunk in size from %d to %d. This can't be handled", info.Name(), oldFileSize, info.Size())
			}
		}

		if info == nil {
			info, err = folder[i].Info()
			if err != nil {
				log.Fatalf("Could not read file info of %s: %v", folder[i].Name(), err)
			}
		}
		knownFileSizes[info.Name()] = info.Size()

		if !ignore && waitForNextFile {
			if !has {
				continue
			}
			if i+1 < len(folder) && isPcapFile(folder[i+1].Name()) != 0 {
				// another file exists and is a pcap file
				log.Printf("File %s is not last next file: %s", folder[i].Name(), folder[i+1].Name())
			} else {
				log.Printf("File %s is last pcap file, waiting for next", folder[i].Name())
				continue
			}
		}

		files = append(files, info.Name())
	}
	return files
}
