package main

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

	_ "github.com/mattn/go-sqlite3"
)

type DataInput interface {
	ReadOne() (float64, error)
	ReadAll() []float64
	ReadBlocks(blockSize int, valChan []chan []float64)
	Close()
}

type csvInput struct {
	fd             *os.File
	inCsv          *csv.Reader
	histColIndex   int
	filterColIndex int
	filterRegex    string
}

var CSV_FILTERED = errors.New("Line filtered, no data for this line")

func DataInput_Csv(inFile string, readCol string, filterCol string, filterRegex string) DataInput {
	in := &csvInput{
		filterRegex: filterRegex,
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
	column = strings.ToLower(strings.TrimSpace(column))
	filterColumn = strings.ToLower(strings.TrimSpace(filterColumn))
	for i, c := range line {
		c = strings.ToLower(strings.TrimSpace(c))
		if c == column {
			in.histColIndex = i
		}
		if c == filterColumn {
			in.filterColIndex = i
		}
	}
	if in.histColIndex == -1 {
		log.Fatalf("Could not find column %s", column)
	} else if in.filterColIndex == -1 {
		log.Fatalf("Could not find filter column %s", filterColumn)
	} else {
		log.Printf("Using column index %d and filtering by %d", in.histColIndex, in.filterColIndex)
	}

	return in
}

func (in *csvInput) readNext() (line []string, err error) {
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

func (in *csvInput) ReadOne() (float64, error) {
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
			if !ignore0 || val != 0 {
				return val * valueFactor, nil
			}
		}
	}
	return 0, CSV_FILTERED
}

func (in *csvInput) ReadBlocks(blockSize int, valChan []chan []float64) {
	var numFiltered = 0
	var numUsed = 0
	values := make([]float64, 0)

	var err error
	var val float64
	for val, err = in.ReadOne(); err == nil || err == CSV_FILTERED; val, err = in.ReadOne() {
		if err == CSV_FILTERED {
			numFiltered++
		} else {
			values = append(values, val*valueFactor)
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

func (in *csvInput) ReadAll() (values []float64) {
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

func (i *csvInput) Close() {
	i.fd.Close()
}

type sqliteInput struct {
	db          *sql.DB
	stmt        *sql.Stmt
	orderedStmt *sql.Stmt
	offset      int
	count       int
	filterRegex string
}

func DataInput_Sqlite(inFile string, readColunm string, filterColumn string, filterRegex string) DataInput {
	in := &sqliteInput{
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

	cmd := fmt.Sprintf("select %s from %s %s limit ?,?;", readCol, readTbl, filterClause)
	log.Println(cmd)
	in.stmt, err = in.db.Prepare(cmd)
	if err != nil {
		log.Fatalln("Could not prepare select statement: ", err)
	}
	in.orderedStmt, err = in.db.Prepare(fmt.Sprintf("select %s from %s %s order by %s;", readCol, readTbl, filterClause, readCol))
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

func (in *sqliteInput) findColumn(col string) (table, column string) {
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

func (in *sqliteInput) ReadOne() (val float64, err error) {
	err = in.stmt.QueryRow(in.filterRegex, in.offset, 1).Scan(&val)
	if err != nil {
		log.Fatalf("Could not read entry %d of table: %v", in.offset, err)
	}
	in.offset++
	return
}

func (in *sqliteInput) ReadAll() (values []float64) {
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

func (in *sqliteInput) ReadOrdered() (values []float64) {
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

func (in *sqliteInput) ReadBlocks(blockSize int, valChan []chan []float64) {
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

func (in *sqliteInput) Close() {
	in.stmt.Close()
	in.db.Close()
}
