package main

import (
	"database/sql"
	"encoding/csv"
	"log"
	"math"
	"os"
	"path"
	"strconv"
	"strings"

	_ "github.com/mattn/go-sqlite3"
)

type DataOutput interface {
	WritePacket(*packetTimings)
	WriteSpeed(*speedData)
	Flush()
	Close()
}

type csvOutput struct {
	file        string
	fd          *os.File
	outCsv      *csv.Writer
	speedFd     *os.File
	speedOutCsv *csv.Writer
}

func openCsv(file string, header []string) (fd *os.File, outCsv *csv.Writer) {
	var err error
	fd, err = os.OpenFile(file, os.O_WRONLY|os.O_APPEND|os.O_CREATE, os.ModePerm)
	if err != nil {
		log.Fatalln("Could not open output file: ", err)
	}

	outCsv = csv.NewWriter(fd)
	if outCsv == nil {
		log.Fatalln("Csv writer could not be inititlized")
	}
	outInfo, err := fd.Stat()
	if err != nil {
		log.Printf("Not adding header to output csv file as existing size could not be determined")
	} else if outInfo.Size() <= 0 {
		outCsv.Write(header)
		outCsv.Flush()
	}
	return
}

func DataOutput_Csv(file string) DataOutput {
	o := &csvOutput{file: file}
	o.fd, o.outCsv = openCsv(file, []string{"Stream", "Index", "Enqueue", "Eligibility", "Transmission", "HwTransmission", "t-e", "hwT-e", "Arrival"})
	return o
}

func PacketToCsv(t *packetTimings) []string {
	return []string{
		strconv.FormatUint(uint64(t.stream), 10), strconv.FormatUint(uint64(t.i), 10),
		strconv.FormatUint(uint64(t.enqueue), 10), strconv.FormatUint(uint64(t.eligibility), 10), strconv.FormatUint(uint64(t.transmission), 10), strconv.FormatUint(uint64(t.hwTransmission), 10),
		strconv.FormatInt(int64(t.DeltaET()), 10), strconv.FormatInt(int64(t.DeltaEHw()), 10),
		strconv.FormatUint(uint64(t.arrival.UnixNano()), 10)}
}

func (o *csvOutput) WritePacket(p *packetTimings) {
	o.outCsv.Write(PacketToCsv(p))
}

func (o *csvOutput) WriteSpeed(p *speedData) {
	if o.speedOutCsv == nil {
		o.speedFd, o.speedOutCsv = openCsv(
			strings.TrimSuffix(o.file, path.Ext(o.file))+"_speed"+path.Ext(o.file),
			[]string{"IntervalStartTimeMs", "NumberOfBytes", "NumberOfBits", "AvgSpeed"},
		)
	}
	if math.IsNaN(p.avgSpeed) {
		o.speedOutCsv.Write([]string{strconv.FormatInt(p.intervalStart.UnixMilli(), 10), strconv.FormatUint(p.numBytes, 10), "-", "INTERVAL_TOO_BIG"})
	} else {
		o.speedOutCsv.Write([]string{strconv.FormatInt(p.intervalStart.UnixMilli(), 10), strconv.FormatUint(p.numBytes, 10), strconv.FormatUint(p.numBits, 10), strconv.FormatFloat(p.avgSpeed, 'e', 3, 64)})
	}
}

func (o *csvOutput) Flush() {
	o.outCsv.Flush()
	if o.speedOutCsv != nil {
		o.speedOutCsv.Flush()
	}
}

func (o *csvOutput) Close() {
	o.Flush()
	o.fd.Close()
	if o.speedFd != nil {
		o.speedFd.Close()
	}
}

type sqliteOutput struct {
	db        *sql.DB
	tx        *sql.Tx
	stmt      *sql.Stmt
	speedStmt *sql.Stmt
}

func DataOutput_Sqlite(file string) DataOutput {
	o := &sqliteOutput{}

	var err error
	o.db, err = sql.Open("sqlite3", file)
	if err != nil {
		log.Fatalln("Could not open sqlite file: ", err)
	}

	_, err = o.db.Exec("create table if not exists packets (id integer not null primary key, stream integer not null, enqueue integer not null, eligibility integer not null, transmission integer not null, hw integer not null, delta_t_e integer not null, delta_hw_e integer not null, arrival integer not null);")
	if err != nil {
		log.Fatalln("Could not create sqlite table: ", err)
	}

	_, err = o.db.Exec("create table if not exists speeds (start_time_ms integer not null primary key, num_bytes integer not null, num_bits integer null, speed real null);")
	if err != nil {
		log.Fatalln("Could not create sqlite speed table: ", err)
	}
	return o
}

func (o *sqliteOutput) ensureTx() {
	if o.tx == nil {
		var err error
		o.tx, err = o.db.Begin()
		if err != nil {
			log.Fatalln("Could not start sqlite transaction: ", err)
		}
		o.stmt, err = o.tx.Prepare("insert into packets values (?, ?, ?, ?, ?, ?, ?, ?, ?);")
		if err != nil {
			log.Fatalln("Could not prepare insert query: ", err)
		}
		o.speedStmt, err = o.tx.Prepare("insert into speeds values (?, ?, ?, ?);")
		if err != nil {
			log.Fatalln("Could not prepare insert query: ", err)
		}
	}
}

func (o *sqliteOutput) WritePacket(p *packetTimings) {
	o.ensureTx()
	_, err := o.stmt.Exec(p.i, p.stream, p.enqueue, p.eligibility, p.transmission, p.hwTransmission, p.DeltaET(), p.DeltaEHw(), p.arrival)
	if err != nil {
		log.Fatalln("Could not insert packet: ", err)
	}
}

func (o *sqliteOutput) WriteSpeed(p *speedData) {
	o.ensureTx()
	_, err := o.speedStmt.Exec(p.intervalStart.UnixMilli(), p.numBytes, p.numBits, p.avgSpeed)
	if err != nil {
		log.Fatalln("Could not insert speed: ", err)
	}
}

func (o *sqliteOutput) Flush() {
	if o.tx != nil {
		err := o.tx.Commit()
		o.tx = nil
		if err != nil {
			log.Fatalln("Could not commit transaction: ", err)
		}
		o.stmt.Close()
		o.stmt = nil
	}
}

func (o *sqliteOutput) Close() {
	o.Flush()
	if o.stmt != nil {
		o.stmt.Close()
	}
	o.db.Close()
}
