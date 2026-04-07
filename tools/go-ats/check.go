package main

import (
	"encoding/csv"
	"log"
	"os"
	"strconv"
	"time"
)

var logFile *csv.Writer

func check_init() {
	f, err := os.Create("log.csv")
	if err != nil {
		log.Fatalln(err)
	}
	logFile = csv.NewWriter(f)
	if err := logFile.Write([]string{"App ID", "Timestamp", "Time us", "Token count", "Event", "Queue length"}); err != nil {
		log.Fatalln(err)
	}
}

func check_deinit() {
	logFile.Flush()
	if err := logFile.Error(); err != nil {
		log.Fatal(err)
	}
}

type check_bucket struct {
	tokens     int
	tokenRate  float64
	bucketSize int
	lastUpdate time.Time
}

var check_buckets = []*check_bucket{{bucketSize: 200, tokenRate: 30 / 5}, {bucketSize: 400}}

type check_event int

const (
	check_event_unknown check_event = iota
	check_event_send
	check_event_enqueue
	check_event_discard
)

func check(pkg []byte, id int, now int64, event check_event, queueSize int) {
	strId := strconv.FormatInt(int64(id), 10)

	beforeNow := time.Unix((now)/int64(time.Second), (now)%int64(time.Second))
	b := check_buckets[id]
	if b.lastUpdate.IsZero() {
		b.tokens = b.bucketSize
	} else {
		deltaT := beforeNow.Sub(b.lastUpdate)
		newTokens := min(b.bucketSize, b.tokens+int(b.tokenRate*float64(deltaT)/float64(time.Second)))
		if newTokens == b.bucketSize && b.tokens < b.bucketSize {
			timeToFull := time.Duration(float64(b.bucketSize-b.tokens) * float64(time.Second) / b.tokenRate)
			fullT := b.lastUpdate.Add(timeToFull)
			logFile.Write([]string{strId, fullT.Format(time.RFC3339Nano), strconv.FormatInt(fullT.UnixMicro(), 10), strconv.FormatInt(int64(b.bucketSize), 10), "Bucket full", ""})
		}
		b.tokens = newTokens
	}
	b.lastUpdate = beforeNow
	eventStr := "Fill"
	qLenStr := ""
	if queueSize >= 0 {
		qLenStr = strconv.FormatInt(int64(queueSize), 10)
	}
	switch event {
	case check_event_enqueue:
		eventStr = "Enqueue"
	case check_event_discard:
		eventStr = "Discard"
	case check_event_send:
		eventStr = ""
	}
	logFile.Write([]string{strId, beforeNow.Format(time.RFC3339Nano), strconv.FormatInt(beforeNow.UnixMicro(), 10), strconv.FormatInt(int64(b.tokens), 10), eventStr, qLenStr})

	if event == check_event_send {
		b.tokens -= len(pkg)
		b.lastUpdate = beforeNow
		logFile.Write([]string{strId, beforeNow.Format(time.RFC3339Nano), strconv.FormatInt(beforeNow.UnixMicro(), 10), strconv.FormatInt(int64(b.tokens), 10), "Send", ""})
	}

	logFile.Flush()
}
