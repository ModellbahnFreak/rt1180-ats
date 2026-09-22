package main

import (
	"encoding/csv"
	"flag"
	"log"
	"math"
	"os"
	"path"
	"slices"
	"strconv"
	"strings"
	"sync"
)

var (
	in           string  = "timings.csv"
	out          string  = ""
	binStart     float64 = 0
	binSize      float64 = 25
	column       string  = "0"
	filterColumn string  = ""
	filterRegex  string  = ""
	ignore0      bool    = true
	write0       bool    = false
	valueFactor  float64 = 1
	blockSize    int     = -2
	calcBoxplot  bool    = true
)

func init() {
	flag.StringVar(&in, "in", in, "Input csv file")
	flag.StringVar(&out, "out", out, "Output csv file. Empty to use input name concatenated with _hist")
	flag.Float64Var(&binStart, "start", binStart, "Left border of first bin")
	flag.Float64Var(&binSize, "size", binSize, "Width of bins")
	flag.StringVar(&column, "column", column, "Column to calculate histogram for. Can be column name or 0 based index")
	flag.StringVar(&filterColumn, "filterColumn", filterColumn, "Column to filter for. Empty to disable filtering")
	flag.StringVar(&filterRegex, "filterRegex", filterRegex, "Regex the filter column must match")
	flag.BoolVar(&ignore0, "ignore0", ignore0, "If flag is set, all 0 values in the value column will be ignored")
	flag.BoolVar(&write0, "write0", write0, "If flag is set, empty bucktes will be written to the output, else they will be left out")
	flag.Float64Var(&valueFactor, "factor", valueFactor, "A factor to apply to all values before histogram calculation")
	flag.IntVar(&blockSize, "block", blockSize, "Number of lines to process at a time. -1 processes all lines at once (!RAM!)")
	flag.BoolVar(&calcBoxplot, "boxplot", calcBoxplot, "Set this flag to false to disable boxplot calculation (Needs whole dataset in ram)")
}

type BoxPlot struct {
	minimum,
	lowerW,
	quartile1,
	median,
	quartile3,
	upperW,
	maximum float64
}

type Histogram struct {
	data      map[int]float64
	minBucket int
	maxBucket int
}

func (b BoxPlot) ToFloats() []float64 {
	return []float64{b.minimum, b.lowerW, b.quartile1, b.median, b.quartile3, b.upperW, b.maximum}
}

func main() {
	flag.Parse()

	var hist *Histogram
	var boxPlot *BoxPlot
	var values []float64

	var reader DataInput
	if strings.HasSuffix(strings.ToLower(in), ".db") {
		reader = DataInput_Sqlite(in, column, filterColumn, filterRegex)
		if blockSize < -1 {
			blockSize = -1
		}
	} else {
		reader = DataInput_Csv(in, column, filterColumn, filterRegex)
		if blockSize < -1 {
			blockSize = 500000
		}
	}

	if blockSize > 0 {
		valCh := make([]chan []float64, 0, 2)
		valCh = append(valCh, make(chan []float64))
		var wg sync.WaitGroup
		wg.Go(func() {
			for v := range valCh[0] {
				hist, _ = updateHistogram(v, hist)
			}
		})
		if calcBoxplot {
			valCh = append(valCh, make(chan []float64))
			wg.Go(func() {
				for v := range valCh[1] {
					boxPlot, values = updateBoxPlot(v, values, boxPlot, false)
				}
			})
		}
		reader.ReadBlocks(blockSize, valCh)
		wg.Wait()
		log.Println("Writing output")
		if boxPlot == nil {
			writeHistogram(hist)
		} else {
			writeHistogram(hist, boxPlot.ToFloats()...)
		}
	} else {
		var values []float64
		var ordered bool = false
		if sqlR, is := reader.(*sqliteInput); is {
			values = sqlR.ReadOrdered()
			ordered = true
		} else {
			values = reader.ReadAll()
		}
		hist, _ := updateHistogram(values, nil)
		if calcBoxplot {
			boxplot, _ := updateBoxPlot(values, nil, nil, ordered)
			writeHistogram(hist, boxplot.ToFloats()...)
		} else {
			writeHistogram(hist)
		}
	}
}

func updateHistogram(values []float64, histBuckets *Histogram) (resBuckets *Histogram, countTotal uint64) {
	if histBuckets == nil {
		histBuckets = &Histogram{
			data:      make(map[int]float64, 0),
			minBucket: math.MaxInt,
			maxBucket: math.MinInt,
		}
	}

	for _, val := range values {
		countTotal++
		i := int(math.Floor((val - binStart) / binSize))
		if _, has := histBuckets.data[i]; !has {
			histBuckets.data[i] = 0
		}
		histBuckets.data[i] += 1
		if i < histBuckets.minBucket {
			histBuckets.minBucket = i
		}
		if i > histBuckets.maxBucket {
			histBuckets.maxBucket = i
		}
	}
	log.Printf("Updated histogram with %d new entries", countTotal)
	return histBuckets, countTotal
}

func updateBoxPlot(newValues []float64, oldValues []float64, oldBoxPlot *BoxPlot, isSorted bool) (boxPlot *BoxPlot, values []float64) {

	boxPlot = oldBoxPlot
	if len(newValues) == 0 {
		values = oldValues
		return
	}
	if boxPlot == nil {
		boxPlot = &BoxPlot{}
	}

	if len(oldValues) == 0 {
		log.Printf("Sorting initial %d values", len(newValues))
		values = make([]float64, len(newValues))
		copy(values, newValues)
		slices.Sort(values)
	} else {
		log.Printf("Adding %d values to existing %d values", len(newValues), len(oldValues))
		values = make([]float64, len(oldValues)+len(newValues))
		copy(values, oldValues)
		copy(values[len(oldValues):], newValues)
		slices.Sort(values)
	}

	len := len(values)

	boxPlot.minimum = values[0]
	boxPlot.maximum = values[len-1]
	if len%2 == 0 {
		boxPlot.median = (values[len/2-1] + values[len/2]) * 0.5
	} else {
		boxPlot.median = values[(len-1)/2]
	}
	if len%4 == 0 {
		boxPlot.quartile1 = (values[len/4-1] + values[len/4]) * 0.5
		boxPlot.quartile3 = (values[len*3/4-1] + values[len*3/4]) * 0.5
	} else {
		boxPlot.quartile1 = values[len/4]
		boxPlot.quartile3 = values[len*3/4]
	}
	irq := boxPlot.quartile3 - boxPlot.quartile1
	var i int
	for i = 0; i < len && values[i] <= (boxPlot.quartile1-1.5*irq); i++ {
	}
	boxPlot.lowerW = values[min(i, len-1)]
	for i = 0; i < len && values[i] < (boxPlot.quartile1+1.5*irq); i++ {
	}
	boxPlot.upperW = values[min(i-1, len-1)]
	log.Printf("Min: %f, lowerW: %f, 0.25: %f, median: %f, 0.75: %f, upperW: %f max: %f", boxPlot.minimum, boxPlot.lowerW, boxPlot.quartile1, boxPlot.median, boxPlot.quartile3, boxPlot.upperW, boxPlot.maximum)
	return
}

func writeHistogram(histBuckets *Histogram, addData ...float64) {
	newFileName := strings.TrimSuffix(in, path.Ext(in)) + "_hist.csv"
	f, err := os.Create(newFileName)
	if err != nil {
		log.Fatalf("Could not open output file %s: %v", newFileName, err)
	}
	defer f.Close()

	csvOut := csv.NewWriter(f)
	defer csvOut.Flush()

	header := []string{"BucketStartIncl", "BucketEndExcl", "Count"}
	for i := range addData {
		header = append(header, strconv.FormatFloat(addData[i], 'f', -1, 64))
	}
	csvOut.Write(header)
	for i := histBuckets.minBucket; i <= histBuckets.maxBucket; i++ {
		if val, has := histBuckets.data[i]; has || write0 {
			bucketStart := binStart + float64(i)*binSize
			if !has {
				val = 0
			}
			csvOut.Write([]string{strconv.FormatFloat(bucketStart, 'f', -1, 64), strconv.FormatFloat(bucketStart+binSize, 'f', -1, 64), strconv.FormatInt(int64(val), 10)})
		}
	}
}
