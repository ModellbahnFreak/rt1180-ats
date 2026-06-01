package main

import (
	"encoding/binary"
	"encoding/csv"
	"flag"
	"fmt"
	"log"
	"os"
	"strconv"
	"time"
)

type timeEvent struct {
	t       time.Duration
	relT    time.Duration
	success bool
}

const timeFlagMask uint64 = 0x7fffffffffffffff

var (
	inFile         string = "debug/speedtest.bin"
	startTimestamp int64  = 0
	ticksPerSecond        = int64(time.Second / time.Nanosecond)
	numInstances          = 5
	numSteps              = 3
	binOrder              = binary.LittleEndian
	binSize               = binary.Size(startTimestamp)
)

func init() {
	flag.StringVar(&inFile, "file", inFile, "Path to the binary dump file of the timings (uint64 array). Run 'dump binary value "+inFile+" speedtest_send_timestamps' in the debugger to dump the data")
	flag.Int64Var(&startTimestamp, "start", startTimestamp, "Start timestamp to which all the others are relative. Not required")
	flag.Int64Var(&ticksPerSecond, "frequency", ticksPerSecond, "The frequency of the time (in ticks per second/Hz). Assumed to be 1GHz (=nanoseconds)")
	flag.IntVar(&numInstances, "instances", numInstances, "(Max) Number of speedtest instrances represented in the data.")
}

func main() {
	flag.Parse()

	data, err := os.ReadFile(inFile)
	if err != nil {
		log.Fatalln("could not read file ", inFile, err)
	}

	requiredMod := (binSize * numInstances * numSteps)
	if len(data)%requiredMod != 0 {
		log.Fatalf("binary data not properly formatted. Length %d not divisible by %d (=%d (uint64) * %d (numInstances) * %d (numSteps))", len(data), requiredMod, binSize, numInstances, numSteps)
	}

	var threadTiming [][]timeEvent = make([][]timeEvent, numInstances)
	for thread := 0; thread < numInstances; thread++ {
		threadTiming[thread] = make([]timeEvent, len(data)/binSize/numInstances)
	}
	var firstEmpty []int = make([]int, numInstances)

	smallestIndex := -1
	var smallestValue uint64 = 0
	for i := 0; i < len(data); i += binSize {
		v := binOrder.Uint64(data[i:])
		if v != 0 && (smallestIndex == -1 || v < smallestValue) {
			smallestIndex = i
			smallestValue = v
		}
	}
	smallestIteration := int(smallestIndex / requiredMod)
	log.Printf("Smallest index %d (=> iteration %d) with value %d", smallestIndex, smallestValue, smallestIteration)
	smallestIndex = smallestIteration * requiredMod

	outIndex := 0
	timeFactor := time.Second / time.Duration(ticksPerSecond)
	for i := smallestIndex; i != smallestIndex || outIndex == 0; i = (i + (binSize * numInstances)) % len(data) {
		for thread := 0; thread < numInstances; thread++ {
			v := binOrder.Uint64(data[i+(thread*binSize):])
			if v != 0 {
				firstEmpty[thread] = outIndex + 1
			}
			threadTiming[thread][outIndex] = timeEvent{
				t:       time.Duration((v&timeFlagMask)+uint64(startTimestamp)) * timeFactor,
				success: (v & (1 << 63)) != 0,
			}
			if outIndex > 0 {
				threadTiming[thread][outIndex].relT = threadTiming[thread][outIndex].t - threadTiming[thread][outIndex-1].t
			}
			fmt.Println(v & timeFlagMask)
		}
		outIndex++
	}

	outFile, err := os.Create(inFile + ".csv")
	var csvFile *csv.Writer = nil
	if err == nil {
		csvFile = csv.NewWriter(outFile)
	}
	fmt.Println("TIMINGS:")
	for thread := 0; thread < numInstances; thread++ {
		if firstEmpty[thread] == 0 {
			continue
		}
		if csvFile != nil {
			csvFile.Write([]string{fmt.Sprintf("Thread %d", thread), "Packet", "Step", "Absolute time", "Rel time", "success"})
		}
		fmt.Printf("Thread %d Packet/Step, Absolute time (ns), Rel to prev (ns), success\n", thread)
		for i := 0; i < len(threadTiming[thread]) && i < firstEmpty[thread]; i += numSteps {
			var relSum time.Duration = 0
			var hadSuccess = false
			for s := range numSteps {
				v := threadTiming[thread][i+s]
				if csvFile != nil {
					csvFile.Write([]string{strconv.Itoa(i + s), strconv.Itoa(i / numSteps), strconv.Itoa(s), strconv.FormatUint(uint64(v.t), 10), strconv.FormatUint(uint64(v.relT), 10), strconv.FormatBool(v.success)})
				}
				fmt.Printf("Step %d/%d, %v, %v, %v\n", i/numSteps, s, v.t, v.relT, v.success)
				relSum += v.relT
				if v.success {
					hadSuccess = true
				}
			}
			if csvFile != nil {
				csvFile.Write([]string{"", strconv.Itoa(i / numSteps), strconv.Itoa(numSteps), "", strconv.FormatUint(uint64(relSum), 10), strconv.FormatBool(hadSuccess)})
			}
			fmt.Printf("Packet %d total, -, %v, %v\n", i/numSteps, relSum, hadSuccess)
		}
		fmt.Println()
	}
	if csvFile != nil {
		csvFile.Flush()
		outFile.Close()
	}
}
