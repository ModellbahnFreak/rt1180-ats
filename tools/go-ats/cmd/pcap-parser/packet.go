package main

import (
	"encoding/binary"
	"log"
	"strconv"
	"time"

	"github.com/google/gopacket"
)

type packetTimings struct {
	stream         uint16
	i              uint64
	enqueue        time.Duration
	eligibility    time.Duration
	transmission   time.Duration
	hwTransmission time.Duration
	arrival        time.Time
}

var packetsWithoutHwTime map[uint16]map[uint64]*packetTimings = make(map[uint16]map[uint64]*packetTimings, 5)
var numHwErrors = 0

func (t packetTimings) ToCsv() []string {
	delta := t.transmission - t.eligibility
	var hwDelta time.Duration
	if t.hwTransmission > 0 {
		hwDelta = t.hwTransmission - t.eligibility
	}
	return []string{
		strconv.FormatUint(uint64(t.stream), 10), strconv.FormatUint(uint64(t.i), 10),
		strconv.FormatUint(uint64(t.enqueue), 10), strconv.FormatUint(uint64(t.eligibility), 10), strconv.FormatUint(uint64(t.transmission), 10), strconv.FormatUint(uint64(t.hwTransmission), 10),
		strconv.FormatInt(int64(delta), 10), strconv.FormatInt(int64(hwDelta), 10),
		strconv.FormatUint(uint64(t.arrival.UnixNano()), 10)}
}

func flushPackets(streamId int) (res []*packetTimings) {
	res = make([]*packetTimings, 0, len(packetsWithoutHwTime))
	if streamId < 0 {
		for _, stream := range packetsWithoutHwTime {
			for _, p := range stream {
				res = append(res, p)
			}
			clear(stream)
		}
	} else if stream, has := packetsWithoutHwTime[uint16(streamId)]; has {
		for _, p := range stream {
			res = append(res, p)
		}
		clear(stream)
	}
	return
}

func parsePacket(i int, data []byte, info gopacket.CaptureInfo) (res []*packetTimings) {
	if info.Length < 14 {
		log.Printf("Captured packet %d has no complete ethernet header", i)
		return
	}

	offset := 14
	var correction time.Duration
	if binary.BigEndian.Uint16(data[12:]) == 0x88f7 {
		if len(data) >= offset+16 {
			correction = time.Duration(binary.LittleEndian.Uint64(data[offset+8:]))
		}
		offset = 64
	} else {
		offset = 16
	}
	if len(data) < offset+32 {
		log.Printf("Captured packet %d does not contain all timing information. Expected at least %d bytes, got %d", i, offset+32, len(data))
		return
	}

	p := &packetTimings{
		stream:       binary.LittleEndian.Uint16(data[offset-2:]),
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
		thisStreamMap, has := packetsWithoutHwTime[p.stream]
		if !has {
			thisStreamMap = make(map[uint64]*packetTimings, 50)
			packetsWithoutHwTime[p.stream] = thisStreamMap
		}
		if len(thisStreamMap) >= 50 {
			if numHwErrors < 100 {
				log.Printf("Currently storing %d packets for stream %d without hw timestamp. Clearing to make space", len(thisStreamMap), p.stream)
			}
			numHwErrors++
			res = append(res, flushPackets(int(p.stream))...)
		}
		thisStreamMap[p.i] = p
	}

	if len(data) < offset+56 {
		return
	}
	hwTime := time.Duration(binary.LittleEndian.Uint64(data[offset+32:]))
	hwTimePacketId := binary.LittleEndian.Uint64(data[offset+40:])
	hwTimeStreamId := binary.LittleEndian.Uint16(data[offset+48:])
	if hwTime > 0 && hwTimePacketId > 0 {
		if hwTimeStreamId == p.stream && hwTimePacketId == p.i {
			log.Printf("Stream %d: Packet %d of delivered its own transmit timestamp. Not possible!", p.stream, p.i)
		} else {
			if thisStreamMap, has := packetsWithoutHwTime[hwTimeStreamId]; has {
				if hwP, has := thisStreamMap[hwTimePacketId]; has {
					hwP.hwTransmission = hwTime
					delete(thisStreamMap, hwTimePacketId)
					if hwTimePacketId != p.i || len(res) == 0 {
						res = append(res, hwP)
					}
				} else {
					log.Printf("Stream %d: Packet %d delivered transmit timestamp of unkonwn packet %d; NO MATCH", hwTimeStreamId, p.i, hwTimePacketId)
				}
			} else {
				log.Printf("Packet %d/%d delivered transmit timestamp of unkonwn stream %d packet %d; NO MATCH", p.stream, p.i, hwTimeStreamId, hwTimePacketId)
			}
		}
	}
	return
}
