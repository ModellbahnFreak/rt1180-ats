package main

import (
	"encoding/csv"
	"flag"
	"log"
	"math"
	"os"
	"path"
	"regexp"
	"slices"
	"strconv"
	"strings"
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
}

func main() {
	flag.Parse()

	values := readValues()
	hist := readAndCreateHistogram(values)
	boxplot := calcBoxPlot(values)
	writeHistogram(hist, boxplot...)
}

func readValues() (valuesOrdered []float64) {
	f, err := os.Open(in)
	if err != nil {
		log.Fatalln("Could not open input: ", err)
	}
	defer f.Close()

	csvIn := csv.NewReader(f)
	var line []string
	var histColIndex int = -1
	var filterColIndex int = -1

	var numFiltered = 0
	valuesOrdered = make([]float64, 0)

	if parsedCol, err := strconv.ParseUint(column, 0, 32); err == nil {
		histColIndex = int(parsedCol)
	}
	if filterColumn == "" {
		filterColIndex = -2
	} else if parsedCol, err := strconv.ParseUint(filterColumn, 0, 32); err == nil {
		filterColIndex = int(parsedCol)
	}

	for line, err = csvIn.Read(); err == nil; line, err = csvIn.Read() {
		if len(line) == 0 {
			continue
		}
		if trimed := strings.TrimSpace(line[0]); (len(line) == 1 && len(trimed) == 0) || strings.HasPrefix(trimed, "#") {
			continue
		}

		if histColIndex == -1 || filterColIndex == -1 {
			column = strings.ToLower(strings.TrimSpace(column))
			filterColumn = strings.ToLower(strings.TrimSpace(filterColumn))
			for i, c := range line {
				c = strings.ToLower(strings.TrimSpace(c))
				switch c {
				case column:
					histColIndex = i
				case filterColumn:
					filterColIndex = i
				}
			}
			if histColIndex == -1 {
				log.Fatalf("Could not find column %s", column)
			} else if filterColIndex == -1 {
				log.Fatalf("Could not find column %s", filterColumn)
			} else {
				log.Printf("Using column index %d and filtering by %d", histColIndex, filterColIndex)
			}
		} else if len(line) > histColIndex {
			if filterColIndex >= 0 {
				if match, err := regexp.MatchString(filterRegex, line[filterColIndex]); !match {
					if err != nil {
						log.Printf("Regex error: %v", err)
					}
					numFiltered++
					continue
				}
			}
			if val, err := strconv.ParseFloat(line[histColIndex], 64); err != nil {
				log.Printf("Column %d of line %v could not be parsed as float64: %v", histColIndex, line, err)
			} else {
				if !ignore0 || val != 0 {
					valuesOrdered = append(valuesOrdered, val)
				}
			}
		}
	}
	log.Printf("Ended reading input due to %v. Processed %d rows, Filtered out %d rows", err, len(valuesOrdered), numFiltered)
	return
}

func readAndCreateHistogram(values []float64) (histBuckets []uint64) {
	var countTotal uint64
	histBuckets = make([]uint64, 0)

	for _, val := range values {
		countTotal++
		i := int(math.Floor((val - binStart) / binSize))
		if len(histBuckets) <= i {
			newBuckets := make([]uint64, i+1)
			copy(newBuckets, histBuckets)
			histBuckets = newBuckets
		}
		histBuckets[i] += 1
	}
	log.Printf("Calculated histogram")
	return
}

func calcBoxPlot(values []float64) []float64 {
	log.Printf("Sorting values")
	slices.Sort(values)
	len := len(values)
	if len == 0 {
		return nil
	}

	var minimum, lowerW, quartile1, median, quartile3, upperW, maximum float64
	minimum = values[0]
	maximum = values[len-1]
	if len%2 == 0 {
		median = (values[len/2-1] + values[len/2]) * 0.5
	} else {
		median = values[(len-1)/2]
	}
	if len%4 == 0 {
		quartile1 = (values[len/4-1] + values[len/4]) * 0.5
		quartile3 = (values[len*3/4-1] + values[len*3/4]) * 0.5
	} else {
		quartile1 = values[len/4]
		quartile3 = values[len*3/4]
	}
	irq := quartile3 - quartile1
	var i int
	for i = 0; i < len && values[i] <= (quartile1-1.5*irq); i++ {
	}
	lowerW = values[min(i, len-1)]
	for i = 0; i < len && values[i] < (quartile1+1.5*irq); i++ {
	}
	upperW = values[min(i-1, len-1)]
	log.Printf("Min: %f, lowerW: %f, 0.25: %f, median: %f, 0.75: %f, upperW: %f max: %f", minimum, lowerW, quartile1, median, quartile3, upperW, maximum)
	return []float64{minimum, lowerW, quartile1, median, quartile3, upperW, maximum}
}

func writeHistogram(histBuckets []uint64, addData ...float64) {
	newFileName := strings.TrimSuffix(in, path.Ext(in)) + "_hist" + path.Ext(in)
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
	hadData := false
	for i := range histBuckets {
		if hadData || histBuckets[i] != 0 {
			hadData = true
			bucketStart := binStart + float64(i)*binSize
			csvOut.Write([]string{strconv.FormatFloat(bucketStart, 'f', -1, 64), strconv.FormatFloat(bucketStart+binSize, 'f', -1, 64), strconv.FormatInt(int64(histBuckets[i]), 10)})
		}
	}
}
