package main

import (
	"log"
	"time"
)

type buffer struct {
	write int
	read  int
	buf   [][]byte
	ids   []int
}

var bucketEmptyTime = make([]int64, 10)
var nsPerToken = []time.Duration{5 * time.Second / 30, 100 * time.Second} // usefull in uc-Implementation to avoid division/float?
var burstSize = []int64{200, 400}
var queue = []*buffer{{buf: make([][]byte, 3), ids: make([]int, 3)}}

func calcQLen(q *buffer) int {
	return (((q.write - q.read) + len(q.buf)) % len(q.buf))
}

func checkHeadOfQ(id int) {
	q := queue[id]
	if q.read == q.write {
		return
	}

	newRead := (q.read + 1) % len(q.buf)
	pkg := q.buf[newRead]
	pkgId := q.ids[newRead]

	arrival := time.Now().UnixNano()
	pkgSize := len(pkg)
	pkgRecoveryTime := time.Duration(pkgSize) * nsPerToken[id]
	elegebilityTime := max(arrival, bucketEmptyTime[id]+int64(pkgRecoveryTime))
	emptyToFullTime := burstSize[id] * int64(nsPerToken[id])
	bucketFullTime := bucketEmptyTime[id] + emptyToFullTime

	tokensBefore := float64(arrival-bucketEmptyTime[id]) / float64(nsPerToken[id])

	elegibilityTimeStruct := time.Unix(elegebilityTime/int64(time.Second), elegebilityTime%int64(time.Second))
	emptyTimeStruct := time.Unix(bucketEmptyTime[id]/int64(time.Second), bucketEmptyTime[id]%int64(time.Second))

	if elegebilityTime <= arrival {
		if elegebilityTime > bucketFullTime {
			bucketEmptyTime[id] += int64(pkgRecoveryTime) + (elegebilityTime - bucketFullTime)
		} else {
			bucketEmptyTime[id] += int64(pkgRecoveryTime)
		}
		tokensAfter := min(float64(arrival-bucketEmptyTime[id])/float64(nsPerToken[id]), float64(burstSize[id]))

		log.Printf("packet %d with %d bytes from %d sent at %v, new bucket empty time: %v, tokens in bucket: before %f, after %f", pkgId, pkgSize, id, elegibilityTimeStruct, emptyTimeStruct, tokensBefore, tokensAfter)
		q.read = newRead
		check(pkg, id, arrival, check_event_send, calcQLen(q))
	} else {
		log.Printf("packet %d with %d bytes from %d elegible at %v, new bucket empty time: %v, tokens in bucket: %f", pkgId, pkgSize, id, elegibilityTimeStruct, emptyTimeStruct, tokensBefore)
	}
}

func send(pkg []byte, id int, pkgId int) {
	checkHeadOfQ(id)

	if (queue[id].write+1)%(len(queue[id].buf)) == queue[id].read {
		log.Printf("Discarding frame %d from %d, max queue length reached", pkgId, id)
		check(pkg, id, time.Now().UnixNano(), check_event_discard, calcQLen(queue[id]))
	} else {
		q := queue[id]
		newWrite := (q.write + 1) % len(q.buf)
		q.buf[newWrite] = pkg
		q.ids[newWrite] = pkgId
		q.write = newWrite
		check(pkg, id, time.Now().UnixNano(), check_event_enqueue, calcQLen(q))
		log.Printf("Enqued packet %d from %d, new queue length %d", pkgId, id, calcQLen(q))
	}

	checkHeadOfQ(id)
}

func main() {
	check_init()
	defer check_deinit()

	go traffic(true, 100, 5*time.Second, 0, send)
	//go traffic(false, 300, 10*time.Second, 1, send)

	time.Sleep(1 * time.Minute)
}
