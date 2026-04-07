package main

import (
	"log"
	"math/rand"
	"time"
)

func traffic(regular bool, maxPacket uint, interval time.Duration, id int, send func([]byte, int, int)) {
	maxPacket = min(1500, maxPacket)
	pkg := make([]byte, maxPacket)
	minInterval := 1 * time.Second
	maxInterval := max(interval, minInterval)
	i := 0
	for true {
		if regular {
			log.Printf("%d attempting to send packet with %d bytes", id, maxPacket)
			send(pkg, id, i)
		} else {
			pkgSize := rand.Int31n(int32(maxPacket))
			log.Printf("%d attempting to send packet with %d bytes", id, pkgSize)
			send(pkg[0:pkgSize], id, i)
		}
		i++
		if regular {
			time.Sleep(interval)
		} else {
			time.Sleep(time.Duration(rand.Int63n(int64(maxInterval-minInterval)) + int64(minInterval)))
		}
	}
}
