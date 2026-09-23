package main

import (
	"go-ats/packet"
	"math"
	"time"

	"github.com/google/gopacket"
)

type speed_analyzer struct {
	out               packet.DataOutput
	nextIntervalStart time.Time
	thisIntervalBytes uint64
	bucketSize        time.Duration
	lastI             int
}

func speed_analyzer_new(outFile packet.DataOutput, bucketSizeMs int) *speed_analyzer {
	s := &speed_analyzer{
		out:        outFile,
		bucketSize: time.Duration(bucketSizeMs) * time.Millisecond,
	}
	return s
}

func (s *speed_analyzer) speed_analyzer_addPacket(i int, data []byte, info gopacket.CaptureInfo) {
	for info.Timestamp.Compare(s.nextIntervalStart) >= 0 {
		if info.Timestamp.Sub(s.nextIntervalStart) > 5*time.Second {
			start := s.nextIntervalStart
			if !s.nextIntervalStart.IsZero() {
				s.out.WriteSpeed(&packet.SpeedData{IntervalStart: start, NumBytes: s.thisIntervalBytes, NumBits: 0, AvgSpeed: math.NaN()})
			}
			s.nextIntervalStart = info.Timestamp
		} else {
			numBits := s.thisIntervalBytes * 8
			s.out.WriteSpeed(&packet.SpeedData{IntervalStart: s.nextIntervalStart, NumBytes: s.thisIntervalBytes, NumBits: numBits, AvgSpeed: float64(numBits) / (float64(s.bucketSize) / float64(time.Second))})
			s.nextIntervalStart = s.nextIntervalStart.Add(s.bucketSize)
		}
		s.out.Flush()
		s.thisIntervalBytes = 0
		s.lastI = i
	}
	s.thisIntervalBytes += uint64(info.Length)
}

func (s *speed_analyzer) speed_analyzer_close() {
	s.out.Flush()
}
