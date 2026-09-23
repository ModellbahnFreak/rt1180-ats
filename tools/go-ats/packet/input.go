package packet

import (
	"database/sql"
	"encoding/csv"
	"errors"
	"fmt"
	"log"
	"math"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

type DataInput interface {
	ReadOne() (float64, error)
	ReadAll() []float64
	ReadBlocks(blockSize int, valChan []chan []float64)
	ReadAllPackets() []*PacketTimings
	Close()
}

type CsvInput struct {
	fd             *os.File
	inCsv          *csv.Reader
	histColIndex   int
	filterColIndex int
	filterRegex    string
	ignore0        bool
	valueFactor    float64
	roundFactor    float64
}

var CSV_FILTERED = errors.New("Line filtered, no data for this line")

func DataInput_Csv(inFile string, readCol string, filterCol string, filterRegex string, ignore0 bool, valueFactor float64, roundTo int) DataInput {
	in := &CsvInput{
		filterRegex: filterRegex,
		ignore0:     true,
		valueFactor: valueFactor,
		roundFactor: math.Pow10(roundTo),
	}
	var err error

	in.fd, err = os.Open(inFile)
	if err != nil {
		log.Fatalln("Could not open input: ", err)
	}
	in.inCsv = csv.NewReader(in.fd)

	if parsedCol, err := strconv.ParseUint(readCol, 0, 32); err == nil {
		in.histColIndex = int(parsedCol)
	}
	if filterCol == "" {
		in.filterColIndex = -2
	} else if parsedCol, err := strconv.ParseUint(filterCol, 0, 32); err == nil {
		in.filterColIndex = int(parsedCol)
	}

	line, err := in.readNext()
	readCol = strings.ToLower(strings.TrimSpace(readCol))
	filterCol = strings.ToLower(strings.TrimSpace(filterCol))
	for i, c := range line {
		c = strings.ToLower(strings.TrimSpace(c))
		if c == readCol {
			in.histColIndex = i
		}
		if c == filterCol {
			in.filterColIndex = i
		}
	}
	if in.histColIndex == -1 {
		log.Fatalf("Could not find column %s", readCol)
	} else if in.filterColIndex == -1 {
		log.Fatalf("Could not find filter column %s", filterCol)
	} else {
		log.Printf("Using column index %d and filtering by %d", in.histColIndex, in.filterColIndex)
	}

	return in
}

func (in *CsvInput) readNext() (line []string, err error) {
	for line, err = in.inCsv.Read(); err == nil; line, err = in.inCsv.Read() {
		if len(line) == 0 {
			continue
		}
		if trimed := strings.TrimSpace(line[0]); (len(line) == 1 && len(trimed) == 0) || strings.HasPrefix(trimed, "#") {
			continue
		}
		return
	}
	return
}

func (in *CsvInput) ReadOne() (float64, error) {
	line, err := in.readNext()
	if err != nil {
		return 0, err
	}

	if len(line) > in.histColIndex {
		if in.filterColIndex >= 0 {
			if match, err := regexp.MatchString(in.filterRegex, line[in.filterColIndex]); !match {
				if err != nil {
					log.Printf("Regex error: %v", err)
				}
				return 0, CSV_FILTERED
			}
		}
		if val, err := strconv.ParseFloat(line[in.histColIndex], 64); err != nil {
			log.Printf("Column %d of line %v could not be parsed as float64: %v", in.histColIndex, line, err)
		} else {
			if !in.ignore0 || val != 0 {
				val = val * in.valueFactor
				if in.roundFactor > 0 && in.roundFactor < math.Inf(1) {
					val = math.Round(val*in.roundFactor) / in.roundFactor
				}
				return val, nil
			}
		}
	}
	return 0, CSV_FILTERED
}

func (in *CsvInput) ReadBlocks(blockSize int, valChan []chan []float64) {
	var numFiltered = 0
	var numUsed = 0
	values := make([]float64, 0)

	var err error
	var val float64
	for val, err = in.ReadOne(); err == nil || err == CSV_FILTERED; val, err = in.ReadOne() {
		if err == CSV_FILTERED {
			numFiltered++
		} else {
			values = append(values, val)
			numUsed++
			if len(values) >= blockSize {
				for i := range valChan {
					valChan[i] <- values
				}
				values = make([]float64, 0)
			}
		}
	}

	for i := range valChan {
		valChan[i] <- values
		close(valChan[i])
	}
	log.Printf("Ended reading input due to %v. Processed %d rows, Filtered out %d rows", err, numUsed, numFiltered)
}

func (in *CsvInput) ReadAll() (values []float64) {
	valChan := make(chan []float64)
	go in.ReadBlocks(500000, []chan []float64{valChan})
	for v := range valChan {
		if values == nil {
			values = v
		} else {
			values = append(values, v...)
		}
	}
	return
}

func (i *CsvInput) Close() {
	i.fd.Close()
}

func (in *CsvInput) ReadAllPackets() (values []*PacketTimings) {
	values = make([]*PacketTimings, 0)

	var err error
	var line []string
	for line, err = in.readNext(); err == nil; line, err = in.readNext() {
		stream, err := strconv.ParseUint(line[0], 10, 16)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		i, err := strconv.ParseUint(line[1], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		enqueue, err := strconv.ParseUint(line[2], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		eligibility, err := strconv.ParseUint(line[3], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		transmission, err := strconv.ParseUint(line[4], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		hw, err := strconv.ParseUint(line[5], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		arrival, err := strconv.ParseInt(line[8], 10, 64)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		sendFlags, err := strconv.ParseUint(line[9], 10, 8)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		enqueueIndex, err := strconv.ParseUint(line[10], 10, 8)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		numSpins, err := strconv.ParseUint(line[11], 10, 8)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		size, err := strconv.ParseUint(line[12], 10, 16)
		if err != nil {
			log.Fatalln("Could not parse input:", err)
		}
		values = append(values, &PacketTimings{
			Stream:         uint16(stream),
			I:              i,
			Enqueue:        time.Duration(enqueue),
			Eligibility:    time.Duration(eligibility),
			Transmission:   time.Duration(transmission),
			HwTransmission: time.Duration(hw),
			Arrival:        time.Unix(arrival/int64(time.Second), arrival%int64(time.Second)),
			SendFlags:      uint8(sendFlags),
			EnqueueIndex:   uint8(enqueueIndex),
			NumSpins:       uint8(numSpins),
			Size:           uint16(size),
		})
	}
	return
}

type SqliteInput struct {
	db          *sql.DB
	stmt        *sql.Stmt
	orderedStmt *sql.Stmt
	offset      int
	count       int
	filterRegex string
}

func DataInput_Sqlite(inFile string, readColunm string, filterColumn string, filterRegex string, ignore0 bool, valueFactor float64, roundTo int) DataInput {
	in := &SqliteInput{
		filterRegex: filterRegex,
		offset:      0,
	}
	var err error

	in.db, err = sql.Open("sqlite3", inFile)
	if err != nil {
		log.Fatalln("Could not open database: ", err)
	}
	readTbl, readCol := in.findColumn(readColunm)
	if readTbl == "" {
		log.Fatalf("Could not find column %s in any table", readColunm)
	}

	filterClause := "where ? is not null"
	if filterColumn != "" {
		var filterTbl string
		filterTbl, filterColumn = in.findColumn(filterColumn)
		if filterTbl == "" || filterTbl != readTbl {
			log.Fatalf("Filter column %s not found in same table as read column", filterColumn)
		}
		filterClause = fmt.Sprintf("where %s like ?", filterColumn)
	}
	if ignore0 {
		filterClause += fmt.Sprintf(" and %s is not null and %s != 0", readCol, readCol)
	}
	selectClause := readCol
	if valueFactor != 1 {
		selectClause = fmt.Sprintf("%s*%f", selectClause, valueFactor)
	}
	roundFactor := math.Pow10(roundTo)
	if roundFactor > 0 && roundFactor < math.Inf(1) {
		selectClause = fmt.Sprintf("round((%s)*%f)/%f", selectClause, roundFactor, roundFactor)
	}

	cmd := fmt.Sprintf("select %s from %s %s limit ?,?;", selectClause, readTbl, filterClause)
	log.Println(cmd)
	in.stmt, err = in.db.Prepare(cmd)
	if err != nil {
		log.Fatalln("Could not prepare select statement: ", err)
	}
	in.orderedStmt, err = in.db.Prepare(fmt.Sprintf("select %s from %s %s order by %s;", selectClause, readTbl, filterClause, readCol))
	if err != nil {
		log.Fatalln("Could not prepare ordered select statement: ", err)
	}
	err = in.db.QueryRow(fmt.Sprintf("select count(%s) from %s %s;", readCol, readTbl, filterClause), filterRegex).Scan(&in.count)
	if err != nil {
		log.Fatalln("Could not get count: ", err)
	}
	log.Printf("Expecting up to %d rows", in.count)

	return in
}

func (in *SqliteInput) findColumn(col string) (table, column string) {
	if strings.Contains(col, ",") {
		parts := strings.Split(col, ",")
		table = strings.TrimSpace(parts[0])
		column = strings.TrimSpace(parts[1])
		log.Printf("Using column %s of table %s", column, table)
		return
	}
	col = strings.ToLower(strings.TrimSpace(col))
	tbls, err := in.db.Query("select name, sql from sqlite_schema where type='table' and name not like 'sqlite_%'")
	if err != nil {
		log.Fatalln("No tables in database: ", err)
	}
	defer tbls.Close()
	for tbls.Next() {
		var cmd string
		err = tbls.Scan(&table, &cmd)
		if err != nil {
			log.Fatalln("Could not read tables: ", err)
		}
		if !strings.HasPrefix(strings.ToLower(cmd), "create table") {
			continue
		}
		firstBracket := strings.Index(cmd, "(")
		lastBracket := strings.LastIndex(cmd, ")")
		if firstBracket == -1 || lastBracket == -1 {
			continue
		}
		cmd = cmd[firstBracket+1 : lastBracket]
		cols := strings.Split(cmd, ",")
		for i := range cols {
			column = strings.TrimSpace(cols[i])
			space := strings.IndexFunc(column, func(r rune) bool { return r == '\t' || r == '\n' || r == '\v' || r == '\f' || r == '\r' || r == ' ' })
			if space >= 0 {
				column = column[0:space]
			}
			if strings.ToLower(column) == col {
				log.Printf("Found column %s in table %s", column, table)
				return
			}
		}
	}
	err = tbls.Err()
	if err != nil {
		log.Fatalln("Error during quering tables: ", err)
	}
	return "", col
}

func (in *SqliteInput) ReadOne() (val float64, err error) {
	err = in.stmt.QueryRow(in.filterRegex, in.offset, 1).Scan(&val)
	if err != nil {
		log.Fatalf("Could not read entry %d of table: %v", in.offset, err)
	}
	in.offset++
	return
}

func (in *SqliteInput) ReadAll() (values []float64) {
	rows, err := in.stmt.Query(in.filterRegex, 0, math.MaxInt)
	if err != nil {
		log.Fatalln("Could not read rows: ", err)
	}
	defer rows.Close()
	in.offset = 0
	values = make([]float64, in.count)
	for rows.Next() {
		err = rows.Scan(&values[in.offset])
		in.offset++
	}
	err = rows.Err()
	if err != nil {
		log.Fatalln("Error during ReadAll: ", err)
	}
	log.Printf("Read %d rows from table", in.offset)
	return
}

func (in *SqliteInput) ReadOrdered() (values []float64) {
	rows, err := in.orderedStmt.Query(in.filterRegex)
	if err != nil {
		log.Fatalln("Could not read rows: ", err)
	}
	defer rows.Close()
	in.offset = 0
	values = make([]float64, in.count)
	for rows.Next() {
		err = rows.Scan(&values[in.offset])
		in.offset++
	}
	err = rows.Err()
	if err != nil {
		log.Fatalln("Error during ReadOrdered: ", err)
	}
	log.Printf("Read %d rows from table", in.offset)
	return
}

func (in *SqliteInput) ReadBlocks(blockSize int, valChan []chan []float64) {
	in.offset = 0
	for {
		rows, err := in.stmt.Query(in.filterRegex, in.offset, blockSize)
		if err != nil {
			log.Fatalf("Could not read rows %d to %d: %v", in.offset, in.offset+blockSize, err)
		}
		values := make([]float64, blockSize)
		i := 0
		for rows.Next() {
			err = rows.Scan(&values[i])
			i++
		}
		if i == 0 {
			break
		}
		in.offset += i
		err = rows.Err()
		if err != nil {
			log.Fatalln("Error during ReadBlocks: ", err)
		}
		values = values[0:i]
		for i := range valChan {
			valChan[i] <- values
		}
		rows.Close()
	}
}

func (in *SqliteInput) ReadAllPackets() (values []*PacketTimings) {
	rows, err := in.db.Query("select id, stream, enqueue, eligibility, transmission, hw, delta_t_e, delta_hw_e, arrival, send_flags, enq_index, num_spins, size from packets order by id asc;")
	if err != nil {
		if strings.Contains(err.Error(), "no such column: size") {
			rows, err = in.db.Query("select id, stream, enqueue, eligibility, transmission, hw, delta_t_e, delta_hw_e, arrival, send_flags, enq_index, num_spins, 0 as size from packets order by id asc;")
			if err != nil {
				log.Fatalln("Could not read packet rows backup: ", err)
			}
		} else {
			log.Fatalln("Could not read packet rows: ", err)
		}
	}
	defer rows.Close()
	in.offset = 0
	values = make([]*PacketTimings, in.count)
	var tmp float64
	for rows.Next() {
		values[in.offset] = &PacketTimings{}
		var arrival string
		err = rows.Scan(
			&values[in.offset].I, &values[in.offset].Stream,
			&values[in.offset].Enqueue, &values[in.offset].Eligibility, &values[in.offset].Transmission, &values[in.offset].HwTransmission,
			&tmp, &tmp, &arrival,
			&values[in.offset].SendFlags, &values[in.offset].EnqueueIndex, &values[in.offset].NumSpins, &values[in.offset].Size,
		)
		if err != nil {
			log.Fatalln("Error scanning packets result: ", err)
		}
		values[in.offset].Arrival, err = time.Parse("2006-01-02 15:04:05.999999999-07:00", arrival)
		if err != nil {
			log.Fatalln("Error parsing arrival time: ", err)
		}
		in.offset++
	}
	err = rows.Err()
	if err != nil {
		log.Fatalln("Error during ReadAllPackets: ", err)
	}
	log.Printf("Read %d rows from table", in.offset)
	return
}

func (in *SqliteInput) Close() {
	in.stmt.Close()
	in.db.Close()
}
