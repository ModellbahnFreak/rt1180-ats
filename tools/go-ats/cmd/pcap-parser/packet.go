package main

import (
	"encoding/binary"
	"log"
	"strconv"
	"time"

	"github.com/google/gopacket"
)

type packetTimings struct {
	i              uint64
	enqueue        time.Duration
	eligibility    time.Duration
	transmission   time.Duration
	hwTransmission time.Duration
	arrival        time.Time
}

var packetsWithoutHwTime map[uint64]*packetTimings = make(map[uint64]*packetTimings, 50)

func (t packetTimings) ToCsv() []string {
	delta := t.transmission - t.eligibility
	var hwDelta time.Duration
	if t.hwTransmission > 0 {
		hwDelta = t.hwTransmission - t.eligibility
	}
	return []string{
		strconv.FormatUint(uint64(t.i), 10),
		strconv.FormatUint(uint64(t.enqueue), 10), strconv.FormatUint(uint64(t.eligibility), 10), strconv.FormatUint(uint64(t.transmission), 10), strconv.FormatUint(uint64(t.hwTransmission), 10),
		strconv.FormatInt(int64(delta), 10), strconv.FormatInt(int64(hwDelta), 10),
		strconv.FormatUint(uint64(t.arrival.UnixNano()), 10)}
}

func flushPackets() (res []*packetTimings) {
	res = make([]*packetTimings, 0, len(packetsWithoutHwTime))
	for _, p := range packetsWithoutHwTime {
		res = append(res, p)
	}
	clear(packetsWithoutHwTime)
	return
}

func parsePacket(i int, data []byte, info gopacket.CaptureInfo) (res []*packetTimings) {
	if info.Length < 44 {
		return
	}

	offset := 14
	var correction time.Duration
	if binary.BigEndian.Uint16(data[12:]) == 0x88f7 {
		correction = time.Duration(binary.LittleEndian.Uint64(data[offset+8:]))
		offset = 64
	}

	p := &packetTimings{
		i:            binary.LittleEndian.Uint64(data[offset+0:]),
		enqueue:      time.Duration(binary.LittleEndian.Uint64(data[offset+8:])),
		eligibility:  time.Duration(binary.LittleEndian.Uint64(data[offset+16:])),
		transmission: time.Duration(binary.LittleEndian.Uint64(data[offset+24:])),
		arrival:      info.Timestamp,
	}
	res = make([]*packetTimings, 0, 1)
	if correction != 0 {
		p.hwTransmission = p.transmission + (correction >> 16)
		res = append(res, p)
	} else {
		if len(packetsWithoutHwTime) >= 50 {
			log.Printf("Currently storing %d packets without hw timestamp. Clearing to make space", len(packetsWithoutHwTime))
			res = append(res, flushPackets()...)
		}
		packetsWithoutHwTime[p.i] = p
	}

	hwTime := time.Duration(binary.LittleEndian.Uint64(data[offset+32:]))
	hwTimePacketId := binary.LittleEndian.Uint64(data[offset+40:])
	if hwTime > 0 && hwTimePacketId > 0 {
		if hwTimePacketId == p.i {
			log.Printf("Packet %d delivered its own transmit timestamp. Not possible!", p.i)
		} else {
			if hwP, has := packetsWithoutHwTime[hwTimePacketId]; has {
				log.Printf("Packet %d delivered transmit timestamp of packet %d; successfully matched", p.i, hwTimePacketId)
				hwP.hwTransmission = hwTime
				delete(packetsWithoutHwTime, hwTimePacketId)
				if hwTimePacketId != p.i || len(res) == 0 {
					res = append(res, hwP)
				}
			} else {
				log.Printf("Packet %d delivered transmit timestamp of unkonwn packet %d; NO MATCH", p.i, hwTimePacketId)
			}
		}
	}
	return
}
