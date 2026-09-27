package main

import (
	"encoding/csv"
	"flag"
	"fmt"
	"go-ats/packet"
	"log"
	"maps"
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
	blockWrite   bool    = true
	calcBoxplot  bool    = true
	roundTo      int     = -400
	normalize    float64 = math.NaN()
	logarithmic  bool    = false
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
	flag.BoolVar(&blockWrite, "blockWrite", blockWrite, "Set this flag in combination with -block to enable writing the current calculation result after every block instead of only at the end")
	flag.BoolVar(&calcBoxplot, "boxplot", calcBoxplot, "Set this flag to false to disable boxplot calculation (Needs whole dataset in ram)")
	flag.IntVar(&roundTo, "round", roundTo, "number of decimal digits to round to (neg before the decimal point); after factor")
	flag.Func("normalize", "Set this flag to normalize hostogram to 1=max count, set it to a number to normalize to that", func(s string) error {
		if s == "" {
			normalize = math.Inf(1)
			return nil
		}
		v, err := strconv.ParseFloat(s, 64)
		if err != nil {
			return fmt.Errorf("Parameter to -normalize must be either nothing (to find max) or a valid float to normalize to: %v", err)
		}
		normalize = v
		return nil
	})
	flag.BoolVar(&logarithmic, "log", logarithmic, "Set this flag to calculate log of values")
}

type BoxPlot struct {
	lowerW,
	quartile1,
	median,
	quartile3,
	upperW float64
	outliers map[float64]bool
}

type Histogram struct {
	data      map[int]float64
	minBucket int
	maxBucket int
	total     int
	mean,
	variance float64
}

func (b BoxPlot) ToFloats() (res []float64) {
	res = make([]float64, len(b.outliers)+5)
	res[0] = b.lowerW
	res[1] = b.quartile1
	res[2] = b.median
	res[3] = b.quartile3
	res[4] = b.upperW
	i := 5
	for v := range b.outliers {
		res[i] = v
		i++
	}
	return
}

func main() {
	flag.Parse()

	var hist *Histogram
	var boxPlot *BoxPlot
	var values []float64

	var reader packet.DataInput
	if strings.HasSuffix(strings.ToLower(in), ".db") {
		reader = packet.DataInput_Sqlite(in, column, filterColumn, filterRegex, ignore0, valueFactor, roundTo)
		if blockSize < -1 {
			blockSize = -1
		}
	} else {
		reader = packet.DataInput_Csv(in, column, filterColumn, filterRegex, ignore0, valueFactor, roundTo)
		if blockSize < -1 {
			blockSize = 500000
		}
	}

	if blockSize > 0 {
		valCh := make([]chan []float64, 0, 2)
		valCh = append(valCh, make(chan []float64, 1))
		var histCh chan Histogram = nil
		var boxplotCh chan BoxPlot = nil
		var wg sync.WaitGroup
		if blockWrite {
			histCh = make(chan Histogram)
			boxplotCh = make(chan BoxPlot)
			wg.Go(func() {
				for h := range histCh {
					log.Println("Writing intermediate histogram")
					writeHistogram(&h)
				}
			})
			wg.Go(func() {
				for b := range boxplotCh {
					log.Println("Writing intermediate boxplot")
					writeBoxplotTex(&b)
				}
			})
		}
		wg.Go(func() {
			for v := range valCh[0] {
				if histCh != nil {
					defer close(histCh)
				}
				hist = updateHistogram(v, hist)
				if histCh != nil {
					histCh <- Histogram{
						data:      maps.Clone(hist.data),
						minBucket: hist.minBucket,
						maxBucket: hist.maxBucket,
						total:     hist.total,
						mean:      hist.mean,
						variance:  hist.variance,
					}
				}
			}
		})
		if calcBoxplot {
			valCh = append(valCh, make(chan []float64, 1))
			wg.Go(func() {
				if boxplotCh != nil {
					defer close(boxplotCh)
				}
				for v := range valCh[1] {
					boxPlot, values = updateBoxPlot(v, values, boxPlot, false)
					if boxplotCh != nil {
						boxplotCh <- *boxPlot
					}
				}
			})
		}
		reader.ReadBlocks(blockSize, valCh)
		wg.Wait()
		if !math.IsNaN(normalize) || logarithmic {
			normalizeHistogram(hist, normalize, logarithmic)
		}
		log.Println("Writing output")
		if boxPlot == nil {
			writeHistogram(hist)
		} else {
			writeHistogram(hist, boxPlot.ToFloats()...)
			writeBoxplotTex(boxPlot)
		}
	} else {
		var values []float64
		var ordered bool = false
		if sqlR, is := reader.(*packet.SqliteInput); is && calcBoxplot {
			values = sqlR.ReadOrdered()
			ordered = true
		} else {
			values = reader.ReadAll()
		}
		hist := updateHistogram(values, nil)
		if !math.IsNaN(normalize) || logarithmic {
			log.Println("Normalizing")
			normalizeHistogram(hist, normalize, logarithmic)
		}
		if calcBoxplot {
			boxPlot, _ = updateBoxPlot(values, nil, nil, ordered)
			writeHistogram(hist, boxPlot.ToFloats()...)
			writeBoxplotTex(boxPlot)
		} else {
			writeHistogram(hist)
		}
	}
}

func updateHistogram(values []float64, histBuckets *Histogram) (resBuckets *Histogram) {
	if histBuckets == nil {
		histBuckets = &Histogram{
			data:      make(map[int]float64, 0),
			minBucket: math.MaxInt,
			maxBucket: math.MinInt,
			mean:      0,
			variance:  0,
		}
	}

	for _, val := range values {
		histBuckets.total++
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
		newMean := histBuckets.mean + (val-histBuckets.mean)/float64(histBuckets.total)
		histBuckets.variance += (val - histBuckets.mean) * (val - newMean)
		histBuckets.mean = newMean
	}
	log.Printf("Updated histogram to %d new entries", histBuckets.total)
	return histBuckets
}

func normalizeHistogram(hist *Histogram, normalize float64, logarithmic bool) {
	if !math.IsNaN(normalize) {
		var maxCount float64 = -1
		if math.IsInf(normalize, 0) {
			for _, c := range hist.data {
				if c > maxCount {
					maxCount = c
				}
			}
		} else {
			maxCount = normalize
		}
		log.Printf("Normalizing to %f", maxCount)
		for i, c := range hist.data {
			hist.data[i] = c / maxCount
		}
	}
	if logarithmic {
		for i, c := range hist.data {
			if c > 0 {
				hist.data[i] = math.Log10(c)
			}
		}
	}
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
	} else {
		log.Printf("Adding %d values to existing %d values", len(newValues), len(oldValues))
		values = make([]float64, len(oldValues)+len(newValues))
		copy(values, oldValues)
		copy(values[len(oldValues):], newValues)
	}
	if !isSorted {
		slices.Sort(values)
	}

	len := len(values)

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
	var lowerWPos, upperWPos int
	for lowerWPos = 0; lowerWPos < len && values[lowerWPos] < (boxPlot.quartile1-1.5*irq); lowerWPos++ {
	}
	lowerWPos = min(lowerWPos, len-1)
	boxPlot.lowerW = values[lowerWPos]
	for upperWPos = 0; upperWPos < len && values[upperWPos] <= (boxPlot.quartile3+1.5*irq); upperWPos++ {
	}
	upperWPos = min(upperWPos-1, len-1)
	boxPlot.upperW = values[upperWPos]
	if boxPlot.outliers == nil {
		boxPlot.outliers = make(map[float64]bool, lowerWPos+(len-upperWPos-1))
	} else {
		clear(boxPlot.outliers)
	}
	for i := 0; i < len; i++ {
		if i < lowerWPos || i > upperWPos {
			boxPlot.outliers[values[i]] = true
		}
	}
	log.Printf("lowerW: %f, 0.25: %f, median: %f, 0.75: %f, upperW: %f, outliers: %+v", boxPlot.lowerW, boxPlot.quartile1, boxPlot.median, boxPlot.quartile3, boxPlot.upperW, boxPlot.outliers)
	return
}

func writeHistogram(histBuckets *Histogram, addData ...float64) {
	newFileName := strings.TrimSuffix(in, path.Ext(in)) + "_hist.csv"
	if len(out) > 0 {
		newFileName = out
	}
	f, err := os.Create(newFileName)
	if err != nil {
		log.Fatalf("Could not open output file %s: %v", newFileName, err)
	}
	defer f.Close()

	csvOut := csv.NewWriter(f)
	defer csvOut.Flush()

	header := []string{"BucketStartIncl", "BucketEndExcl", "Count", "Mean:" + strconv.FormatFloat(histBuckets.mean, 'f', -1, 64), "Var:" + strconv.FormatFloat(histBuckets.variance/(float64(histBuckets.total)-1), 'f', -1, 64)}
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
			csvOut.Write([]string{strconv.FormatFloat(bucketStart, 'f', -1, 64), strconv.FormatFloat(bucketStart+binSize, 'f', -1, 64), strconv.FormatFloat(val, 'f', -1, 64)})
		}
	}
}

func writeBoxplotTex(boxplot *BoxPlot) {
	newFileName := strings.TrimSuffix(in, path.Ext(in)) + "_boxpl.tex"
	if len(out) > 0 {
		newFileName = strings.TrimSuffix(strings.TrimSuffix(out, path.Ext(out)), "_hist") + "_boxpl.tex"
	}
	f, err := os.Create(newFileName)
	if err != nil {
		log.Fatalf("Could not open output file %s: %v", newFileName, err)
	}
	defer f.Close()

	var outliers = make([]float64, 0, len(boxplot.outliers))
	outliersStr := ""
	for v := range boxplot.outliers {
		outliers = append(outliers, v)
	}
	slices.Sort(outliers)
	for i := range outliers {
		outliersStr += strconv.FormatFloat(outliers[i], 'f', -1, 64) + "\\\\"
	}
	f.WriteString(fmt.Sprintf(`\addplot+ [
	boxplot prepared={
			lower whisker=%f,
			lower quartile=%f,
			median=%f,
			upper quartile=%f,
			upper whisker=%f,
		},
]
table [row sep=\\,y index=0] {%s};`, boxplot.lowerW, boxplot.quartile1, boxplot.median, boxplot.quartile3, boxplot.upperW, outliersStr))
}
