package main

import (
	"compress/gzip"
	"encoding/csv"
	"flag"
	"io"
	"io/fs"
	"log"
	"os"
	"path"
	"strings"
	"time"

	"github.com/google/gopacket/pcapgo"
)

var (
	inFile             string = ""
	folderPath         string
	ignoreInitialFiles bool   = true
	waitForNextFile    bool   = true
	outFile            string = "timings.csv"
)

var knownFileSizes map[string]int64 = make(map[string]int64)
var outCsv *csv.Writer = nil

func init() {
	flag.StringVar(&inFile, "in", inFile, "Input pcap(ng) file to parse. If set, no automatic file detection will happen")
	flag.StringVar(&folderPath, "folder", folderPath, "Path to the folder of pcap files to parse")
	flag.BoolVar(&ignoreInitialFiles, "ignore", ignoreInitialFiles, "Set this flag, to ognore all files initially in the dir")
	flag.BoolVar(&waitForNextFile, "nextFile", waitForNextFile, "Set this flag, to only read files once the next file exists")
	flag.StringVar(&outFile, "out", outFile, "Path to the output csv file")
}

func main() {
	flag.Parse()

	if len(inFile) == 0 && ignoreInitialFiles {
		files := findAllPcapFiles(true)
		log.Printf("Ignoring initial files: %v", files)
		for f := range knownFileSizes {
			knownFileSizes[f] = -1
		}
	}

	outF, err := os.OpenFile(outFile, os.O_WRONLY|os.O_APPEND|os.O_CREATE, os.ModePerm)
	if err != nil {
		log.Fatalln("Could not open output file: ", err)
	}
	defer outF.Close()
	outCsv = csv.NewWriter(outF)
	if outCsv == nil {
		log.Fatalln("Csv writer could not be inititlized")
	}
	outInfo, err := outF.Stat()
	if err != nil {
		log.Printf("Not adding header to output csv file as existing size could not be determined")
	} else if outInfo.Size() <= 0 {
		outCsv.Write([]string{"Index", "Enqueue", "Eligibility", "Transmission", "HwTransmission", "t-e", "hwT-e", "Arrival"})
		outCsv.Flush()
	}

	defer func() {
		packets := flushPackets()
		for _, p := range packets {
			outCsv.Write(p.ToCsv())
		}
	}()

	if len(inFile) > 0 {
		processPcapFile(inFile)
	} else {
		t := time.NewTicker(1 * time.Second)
		for range t.C {
			log.Println("Reading folder")
			files := findAllPcapFiles(false)
			for _, f := range files {
				log.Printf("Processing %s", f)
				processPcapFile(path.Join(folderPath, f))
				knownFileSizes[f] = -1
			}
			if !waitForNextFile && !ignoreInitialFiles {
				return
			}
		}
	}
}

func processPcapFile(f string) {
	var reader io.Reader
	fRead, err := os.Open(f)
	if err != nil {
		log.Fatalf("Could not read file %s: %v", f, err)
	}
	defer fRead.Close()
	var has bool
	if f, has = strings.CutSuffix(strings.ToLower(f), ".gz"); has {
		gzRead, err := gzip.NewReader(fRead)
		if err != nil {
			log.Fatalf("Could not ungzip compressed file %s.gz: %v", f, err)
		}
		defer gzRead.Close()
		reader = gzRead
	} else {
		reader = fRead
	}

	if strings.HasSuffix(f, ".pcapng") {
		capRead, err := pcapgo.NewNgReader(reader, pcapgo.DefaultNgReaderOptions)
		if err != nil {
			log.Fatalf("Could not read pcapng file %s: %v", f, err)
		}

		defer outCsv.Flush()
		for i := 0; ; i++ {
			data, info, err := capRead.ZeroCopyReadPacketData()
			if err != nil {
				log.Printf("Finishing reading %s due to %v", f, err)
				return
			}
			packets := parsePacket(i, data, info)
			for _, p := range packets {
				outCsv.Write(p.ToCsv())
			}
		}
	} else {
		log.Fatalln("NOT IMPLEMENTED: pcap files")
	}
}

// 0: no, 0b001: yes, 0b010: gz, 0b100: pcapng
func isPcapFile(f string) (res int) {
	nameLower := strings.ToLower(f)
	var has bool
	if nameLower, has = strings.CutSuffix(nameLower, ".gz"); has {
		res |= 0b010
	}
	if strings.HasSuffix(nameLower, ".pcap") {
		res |= 0b001
	} else if strings.HasSuffix(nameLower, ".pcapng") {
		res |= 0b101
	} else {
		return 0
	}
	return
}

func findAllPcapFiles(ignore bool) []string {
	folder, err := os.ReadDir(folderPath)
	if err != nil {
		log.Fatalln("Could not read files from folder ", folderPath, " ", err)
	}
	files := make([]string, 0)
	for i := range folder {
		if isPcapFile(folder[i].Name()) == 0 {
			continue
		}

		var info fs.FileInfo = nil
		if oldFileSize, has := knownFileSizes[folder[i].Name()]; has {
			if oldFileSize == -1 { // ignore any file which has -1 size set (ignore override)
				continue
			}

			info, err = folder[i].Info()
			if err != nil {
				log.Fatalf("Could not read file info of %s: %v", folder[i].Name(), err)
			}
			if info.Size() > oldFileSize {
				log.Printf("File %s still growing at %d bytes. Ignoring", info.Name(), info.Size())
				knownFileSizes[info.Name()] = info.Size()
				continue
			} else if info.Size() < oldFileSize {
				log.Fatalf("File %s shrunk in size from %d to %d. This can't be handled", info.Name(), oldFileSize, info.Size())
			}
		}

		if info == nil {
			info, err = folder[i].Info()
			if err != nil {
				log.Fatalf("Could not read file info of %s: %v", folder[i].Name(), err)
			}
		}
		knownFileSizes[info.Name()] = info.Size()

		if !ignore && waitForNextFile && !(i+1 < len(folder) && isPcapFile(folder[i+1].Name()) != 0) {
			continue
		}

		files = append(files, info.Name())
	}
	return files
}
