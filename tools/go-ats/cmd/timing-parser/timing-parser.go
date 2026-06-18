package main

import (
	"encoding/binary"
	"encoding/csv"
	"flag"
	"fmt"
	"log"
	"math"
	"os"
	"strconv"
	"time"
)

type timeEvent struct {
	t       time.Duration
	relT    time.Duration
	success bool
}

const timeFlagBits uint64 = 1
const timeFlagMask uint64 = math.MaxUint64 >> timeFlagBits

var (
	inFile         string = "debug/speedtest.bin"
	startTimestamp int64  = 0
	ticksPerSecond        = int64(time.Second / time.Nanosecond)
	numInstances          = 5
	numSteps              = 3
	binOrder              = binary.LittleEndian
	binSize               = binary.Size(startTimestamp)
	verbose               = false
)

func init() {
	flag.StringVar(&inFile, "file", inFile, "Path to the binary dump file of the timings (uint64 array). Run 'dump binary value "+inFile+" speedtest_send_timestamps' in the debugger to dump the data")
	flag.Int64Var(&startTimestamp, "start", startTimestamp, "Start timestamp to which all the others are relative. Not required")
	flag.Int64Var(&ticksPerSecond, "frequency", ticksPerSecond, "The frequency of the time (in ticks per second/Hz). Assumed to be 1GHz (=nanoseconds)")
	flag.IntVar(&numInstances, "instances", numInstances, "(Max) Number of speedtest instrances represented in the data.")
	flag.BoolVar(&verbose, "verbose", verbose, "Increase output verbosity. (Log all times and values)")
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
	smallestIndex := make([]int, numInstances)
	var smallestValue = make([]uint64, numInstances)
	for thread := 0; thread < numInstances; thread++ {
		threadTiming[thread] = make([]timeEvent, len(data)/binSize/numInstances)
		smallestIndex[thread] = -1
	}
	var firstEmpty []int = make([]int, numInstances)

	// find current start of ring buffer (for each thread)
	for i := 0; i < len(data); i += binSize {
		thread := (i / binSize) % numInstances
		v := binOrder.Uint64(data[i:]) & timeFlagMask
		if v != 0 && (smallestIndex[thread] == -1 || v < smallestValue[thread]) {
			smallestIndex[thread] = i
			smallestValue[thread] = v
		}
	}

	for thread := 0; thread < numInstances; thread++ {
		smallestIteration := int(smallestIndex[thread] / requiredMod)
		log.Printf("Thread %d Smallest index %d (=> iteration %d => start %d) with value %d", thread, smallestIndex[thread], smallestIteration, smallestIteration*requiredMod, smallestValue[thread])
		smallestIndex[thread] = smallestIteration * requiredMod
	}

	// Parse all data and write it to threadTiming
	timeFactor := time.Second / time.Duration(ticksPerSecond)
	fixRelTimeMode := false
	if verbose {
		fmt.Println("Original timestamps:")
	}
	for thread := 0; thread < numInstances; thread++ {
		for outIndex := 0; outIndex < len(threadTiming[thread]); outIndex++ {
			i := (smallestIndex[thread] + (thread * binSize) + (outIndex * (binSize * numInstances))) % len(data)
			v := binOrder.Uint64(data[i:])
			if v != 0 {
				firstEmpty[thread] = outIndex + 1
			}
			t := v & timeFlagMask
			flags := (v & (timeFlagMask ^ math.MaxUint64)) >> (64 - timeFlagBits)
			step := outIndex % numSteps
			packet := int(outIndex / numSteps)
			if verbose {
				fmt.Printf("%d/%d/%d: %d, 0x%x\n", thread, packet, step, t, flags)
			}
			threadTiming[thread][outIndex] = timeEvent{
				t:       time.Duration(t+uint64(startTimestamp)) * timeFactor,
				success: flags != 0,
			}
			if outIndex > numSteps || (outIndex > 0 && threadTiming[thread][outIndex].t-threadTiming[thread][outIndex-1].t > 0) {
				threadTiming[thread][outIndex].relT = threadTiming[thread][outIndex].t - threadTiming[thread][outIndex-1].t
				if threadTiming[thread][outIndex].relT < 0 && !fixRelTimeMode {
					log.Printf("Thread %d Step %d/%d has a negative rel time of %v, this is impossible!!", thread, packet, step, threadTiming[thread][outIndex].relT)
					log.Printf("Switching to fixing rel time mode")
					fixRelTimeMode = true
				}
			}
			if outIndex%numSteps != numSteps-1 && flags != 0 {
				log.Fatalf("Thread %d Step %d/%d has flags set outside last step. Likely wrong number of threads!", thread, packet, step)
			}
		}
	}

	// For earlier captures withh bug in time logging: Fix wrongly ordered times
	if fixRelTimeMode {
		var firstToFix timeEvent
		for thread := 0; thread < numInstances; thread++ {
			if firstEmpty[thread] == 0 {
				continue
			}
			for i := 0; i < len(threadTiming[thread]) && i < firstEmpty[thread]; i += numSteps {
				for s := range numSteps {
					switch s {
					case numSteps - 1:
						if i == 0 {
							firstToFix = threadTiming[thread][i+s]
						}
						if i+numSteps == len(threadTiming[thread]) {
							threadTiming[thread][i+s] = firstToFix
						} else {
							threadTiming[thread][i+s] = threadTiming[thread][i+s+numSteps]
						}
						fallthrough
					case 0:
						if s+i > numSteps || (i+s > 0 && threadTiming[thread][i+s].t-threadTiming[thread][i+s-1].t > 0) {
							threadTiming[thread][i+s].relT = threadTiming[thread][i+s].t - threadTiming[thread][i+s-1].t
						} else {
							threadTiming[thread][i+s].relT = 0
						}
					}
					if threadTiming[thread][i+s].relT < 0 {
						log.Fatalf("Thread %d Step %d/%d still has a negative rel time of %v after fixing!! Exiting", thread, int(i/numSteps), s, threadTiming[thread][i+s].relT)
					}
				}
			}
		}
		log.Println("Fixed successfully!")
	}

	// write csv output
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

		minT := time.Duration(math.MaxInt64)
		maxT := time.Duration(math.MinInt64)
		allSuccess := true
		for i := 0; i < len(threadTiming[thread]) && i < firstEmpty[thread]; i += numSteps {
			var relSum time.Duration = 0
			var hadSuccess = false
			for s := range numSteps {
				v := threadTiming[thread][i+s]
				if csvFile != nil {
					csvFile.Write([]string{strconv.Itoa(i + s), strconv.Itoa(i / numSteps), strconv.Itoa(s), strconv.FormatUint(uint64(v.t), 10), strconv.FormatInt(int64(v.relT), 10), strconv.FormatBool(v.success)})
				}
				if verbose {
					fmt.Printf("Step %d/%d, %v, %v, %v\n", i/numSteps, s, v.t, v.relT, v.success)
				}
				if v.relT <= 0 {
					relSum = -1
				} else if relSum >= 0 {
					relSum += v.relT
				}
				if v.success {
					hadSuccess = true
				}
			}
			if csvFile != nil {
				relSumStr := strconv.FormatInt(int64(relSum), 10)
				if relSum < 0 {
					relSumStr = ""
				}
				csvFile.Write([]string{"", strconv.Itoa(i / numSteps), strconv.Itoa(numSteps), "", relSumStr, strconv.FormatBool(hadSuccess)})
			}
			if verbose {
				fmt.Printf("Packet %d total, -, %v, %v\n", i/numSteps, relSum, hadSuccess)
			}
			if relSum < minT {
				minT = relSum
			}
			if relSum > maxT {
				maxT = relSum
			}
			if !threadTiming[thread][i+numSteps-1].success {
				allSuccess = false
			}
		}
		if verbose {
			fmt.Println()
		} else {
			fmt.Printf("Minimum Packet time, - , %v, %v\n", minT, allSuccess)
			fmt.Printf("Maximum Packet time, - , %v, %v\n", maxT, allSuccess)
		}
	}
	if csvFile != nil {
		csvFile.Flush()
		outFile.Close()
	}
}
