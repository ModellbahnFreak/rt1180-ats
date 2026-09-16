package main

import (
	"encoding/csv"
	"log"
	"os"
	"path"
	"strconv"
	"strings"
	"time"

	"github.com/google/gopacket"
)

type speed_analyzer struct {
	outF              *os.File
	speedCsv          *csv.Writer
	nextIntervalStart time.Time
	thisIntervalBytes uint64
	bucketSize        time.Duration
	lastI             int
}

func speed_analyzer_new(outFile string, bucketSizeMs int) *speed_analyzer {
	newFileName := strings.TrimSuffix(outFile, path.Ext(outFile)) + "_speed" + path.Ext(outFile)
	s := &speed_analyzer{
		bucketSize: time.Duration(bucketSizeMs) * time.Millisecond,
	}
	var err error

	s.outF, err = os.OpenFile(newFileName, os.O_WRONLY|os.O_APPEND|os.O_CREATE, os.ModePerm)
	if err != nil {
		log.Fatalln("Could not open speed output file: ", err)
	}
	s.speedCsv = csv.NewWriter(s.outF)
	if s.speedCsv == nil {
		log.Fatalln("Csv writer for speed could not be inititlized")
	}
	outInfo, err := s.outF.Stat()
	if err != nil {
		log.Printf("Not adding header to speed output csv file as existing size could not be determined")
	} else if outInfo.Size() <= 0 {
		s.speedCsv.Write([]string{"IntervalStartTimeMs", "NumberOfBytes", "NumberOfBits", "AvgSpeed"})
		s.speedCsv.Flush()
	}
	return s
}

func (s *speed_analyzer) speed_analyzer_addPacket(i int, data []byte, info gopacket.CaptureInfo) {
	for info.Timestamp.Compare(s.nextIntervalStart) >= 0 {
		if info.Timestamp.Sub(s.nextIntervalStart) > 5*time.Second {
			start := s.nextIntervalStart
			if s.nextIntervalStart.IsZero() {
				start = info.Timestamp
			}
			s.speedCsv.Write([]string{strconv.FormatInt(start.UnixMilli(), 10), strconv.FormatUint(s.thisIntervalBytes, 10), "-", "INTERVAL_TOO_BIG"})
			s.nextIntervalStart = info.Timestamp
		} else {
			numBits := s.thisIntervalBytes * 8
			s.speedCsv.Write([]string{strconv.FormatInt(s.nextIntervalStart.UnixMilli(), 10), strconv.FormatUint(s.thisIntervalBytes, 10), strconv.FormatUint(numBits, 10), strconv.FormatFloat(float64(numBits)/(float64(s.bucketSize)/float64(time.Second)), 'e', 3, 64)})
			s.nextIntervalStart = s.nextIntervalStart.Add(s.bucketSize)
		}
		s.speedCsv.Flush()
		s.thisIntervalBytes = 0
		s.lastI = i
	}
	s.thisIntervalBytes += uint64(info.Length)
}

func (s *speed_analyzer) speed_analyzer_close() {
	s.speedCsv.Flush()
	s.outF.Close()
}
