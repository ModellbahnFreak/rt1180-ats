package main

import (
	"encoding/binary"
	"go-ats/packet"
	"log"
	"time"

	"github.com/google/gopacket"
)

var packetsWithoutHwTime = make(map[uint8]map[uint64]*packet.PacketTimings, 5)
var prevPacket = make(map[uint8]map[uint64]*packet.PacketTimings, 5)
var numHwErrors = 0

var lastSeenId = make(map[uint8]uint64)
var missingIds = make(map[uint8]int)
var duplicateIds = make(map[uint8]int)
var countDropped = 0
var countTotal = make(map[uint8]int)
var headerMissing = 0

func flushPackets(streamId int) (res []*packet.PacketTimings) {
	res = make([]*packet.PacketTimings, 0, len(packetsWithoutHwTime))
	if streamId < 0 {
		for _, stream := range packetsWithoutHwTime {
			for _, p := range stream {
				res = append(res, p)
			}
			clear(stream)
		}
	} else if stream, has := packetsWithoutHwTime[uint8(streamId)]; has {
		for _, p := range stream {
			res = append(res, p)
		}
		clear(stream)
	}
	return
}

func packetMissingReport() {
	log.Println("Missing IDs Report:")
	absTotal := 0
	for s := range lastSeenId {
		absTotal += countTotal[s]
		log.Printf("Stream %d: Last ID: %d, Missing IDs: %d, Duplicate IDs: %d, Stream Total: %d", s, lastSeenId[s], missingIds[s], duplicateIds[s], countTotal[s])
	}
	log.Printf("Dropped status: %d, Total packets: %d", countDropped, absTotal)
	log.Printf("Number of packets with missing ethernet header: %d", headerMissing)
}

func packetCheckMissingDuplicate(p *packet.PacketTimings) {
	if _, has := countTotal[p.Stream]; !has {
		countTotal[p.Stream] = 0
	}
	if _, has := lastSeenId[p.Stream]; !has {
		lastSeenId[p.Stream] = 0
	}
	if _, has := duplicateIds[p.Stream]; !has {
		duplicateIds[p.Stream] = 0
	}
	if _, has := missingIds[p.Stream]; !has {
		missingIds[p.Stream] = 0
	}
	countTotal[p.Stream]++
	if lastSeenId[p.Stream] != 0 {
		if p.I <= lastSeenId[p.Stream] {
			duplicateIds[p.Stream]++
		}
		for ; p.I > lastSeenId[p.Stream]+1; lastSeenId[p.Stream]++ {
			missingIds[p.Stream]++
		}
	}
	lastSeenId[p.Stream] = p.I
}

func parsePacket(i int, data []byte, info gopacket.CaptureInfo) (res []*packet.PacketTimings) {
	if info.Length < 14 {
		log.Printf("Captured packet %d has no complete ethernet header", i)
		return
	}

	var correction time.Duration
	offset := 14
	if (binary.LittleEndian.Uint64(data)&0xffffffffffff) != 0xffffffffffff && (info.Length+offset)%100 == 0 {
		// Frame was sent with wrong offset again, leaving no ethernet header
		offset = 2
		headerMissing++
	} else if binary.BigEndian.Uint16(data[12:]) == 0x88f7 {
		if len(data) >= offset+16 {
			correction = time.Duration(binary.LittleEndian.Uint64(data[offset+8:]))
		}
		offset = 64
	} else {
		offset = 16
	}
	if len(data) < offset+64 {
		log.Printf("Captured packet %d does not contain all timing information. Expected at least %d bytes, got %d", i, offset+64, len(data))
		return
	}

	p := &packet.PacketTimings{
		Stream:       data[offset-2],
		I:            binary.LittleEndian.Uint64(data[offset+0:]),
		Enqueue:      time.Duration(binary.LittleEndian.Uint64(data[offset+8:])),
		Eligibility:  time.Duration(binary.LittleEndian.Uint64(data[offset+16:])),
		Transmission: time.Duration(binary.LittleEndian.Uint64(data[offset+24:])),
		Arrival:      info.Timestamp,
		SendFlags:    data[offset+50] & 0x7f,
		EnqueueIndex: data[offset+51],
		NumSpins:     0,
	}
	for i := 56; i < 64; i++ {
		v := data[offset+i]
		if v != 0xff {
			if (v & 0x80) == 0x80 {
				countDropped++
			}
			p.SendFlags |= (v & 0x80)
			p.NumSpins++
		}
	}
	packetCheckMissingDuplicate(p)

	res = make([]*packet.PacketTimings, 0, 1)
	if correction != 0 {
		p.HwTransmission = p.Transmission + (correction >> 16)
		res = append(res, p)
	} else {
		thisStreamMap, has := packetsWithoutHwTime[p.Stream]
		if !has {
			thisStreamMap = make(map[uint64]*packet.PacketTimings, 50)
			packetsWithoutHwTime[p.Stream] = thisStreamMap
		}
		if len(thisStreamMap) >= 50 {
			if numHwErrors < 100 {
				log.Printf("Currently storing %d packets for stream %d without hw timestamp. Clearing to make space", len(thisStreamMap), p.Stream)
			}
			numHwErrors++
			res = append(res, flushPackets(int(p.Stream))...)
		}
		thisStreamMap[p.I] = p
	}

	if len(data) < offset+56 {
		return
	}
	hwTime := time.Duration(binary.LittleEndian.Uint64(data[offset+32:]))
	hwTimePacketId := binary.LittleEndian.Uint64(data[offset+40:])
	hwTimeStreamId := binary.LittleEndian.Uint16(data[offset+48:])
	if hwTime > 0 && hwTimePacketId > 0 {
		if uint8(hwTimeStreamId) == p.Stream && hwTimePacketId == p.I {
			log.Printf("Stream %d: Packet %d of delivered its own transmit timestamp. Not possible!", p.Stream, p.I)
		} else {
			if thisStreamMap, has := packetsWithoutHwTime[uint8(hwTimeStreamId)]; has {
				if hwP, has := thisStreamMap[hwTimePacketId]; has {
					hwP.HwTransmission = hwTime
					delete(thisStreamMap, hwTimePacketId)
					if hwTimePacketId != p.I || len(res) == 0 {
						res = append(res, hwP)
					}
				} else {
					log.Printf("Stream %d: Packet %d delivered transmit timestamp of unkonwn packet %d; NO MATCH", hwTimeStreamId, p.I, hwTimePacketId)
				}
			} else {
				log.Printf("Packet %d/%d delivered transmit timestamp of unkonwn stream %d packet %d; NO MATCH", p.Stream, p.I, hwTimeStreamId, hwTimePacketId)
			}
		}
	}
	return
}
