package packet

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
	WritePacket(*PacketTimings)
	WriteSpeed(*SpeedData)
	Flush()
	Close()
}

type CsvOutput struct {
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
	o := &CsvOutput{file: file}
	o.fd, o.outCsv = openCsv(file, []string{"Stream", "Index", "Enqueue", "Eligibility", "Transmission", "HwTransmission", "t-e", "hwT-e", "Arrival", "SendFlags", "EnqueueIndex", "NumSpins", "Size"})
	return o
}

func PacketToCsv(t *PacketTimings) []string {
	return []string{
		strconv.FormatUint(uint64(t.Stream), 10), strconv.FormatUint(uint64(t.I), 10),
		strconv.FormatUint(uint64(t.Enqueue), 10), strconv.FormatUint(uint64(t.Eligibility), 10), strconv.FormatUint(uint64(t.Transmission), 10), strconv.FormatUint(uint64(t.HwTransmission), 10),
		strconv.FormatInt(int64(t.DeltaET()), 10), strconv.FormatInt(int64(t.DeltaEHw()), 10),
		strconv.FormatUint(uint64(t.Arrival.UnixNano()), 10),
		strconv.FormatUint(uint64(t.SendFlags), 10), strconv.FormatUint(uint64(t.EnqueueIndex), 10), strconv.FormatUint(uint64(t.NumSpins), 10), strconv.FormatUint(uint64(t.Size), 10),
	}
}

func (o *CsvOutput) WritePacket(p *PacketTimings) {
	o.outCsv.Write(PacketToCsv(p))
}

func (o *CsvOutput) WriteSpeed(p *SpeedData) {
	if o.speedOutCsv == nil {
		o.speedFd, o.speedOutCsv = openCsv(
			strings.TrimSuffix(o.file, path.Ext(o.file))+"_speed"+path.Ext(o.file),
			[]string{"IntervalStartTimeMs", "NumberOfBytes", "NumberOfBits", "AvgSpeed"},
		)
	}
	if math.IsNaN(p.AvgSpeed) {
		o.speedOutCsv.Write([]string{strconv.FormatInt(p.IntervalStart.UnixMilli(), 10), strconv.FormatUint(p.NumBytes, 10), "-", "INTERVAL_TOO_BIG"})
	} else {
		o.speedOutCsv.Write([]string{strconv.FormatInt(p.IntervalStart.UnixMilli(), 10), strconv.FormatUint(p.NumBytes, 10), strconv.FormatUint(p.NumBits, 10), strconv.FormatFloat(p.AvgSpeed, 'e', 3, 64)})
	}
}

func (o *CsvOutput) Flush() {
	o.outCsv.Flush()
	if o.speedOutCsv != nil {
		o.speedOutCsv.Flush()
	}
}

func (o *CsvOutput) Close() {
	o.Flush()
	o.fd.Close()
	if o.speedFd != nil {
		o.speedFd.Close()
	}
}

type SqliteOutput struct {
	db        *sql.DB
	tx        *sql.Tx
	stmt      *sql.Stmt
	speedStmt *sql.Stmt
}

func DataOutput_Sqlite(file string) DataOutput {
	o := &SqliteOutput{}

	var err error
	o.db, err = sql.Open("sqlite3", file)
	if err != nil {
		log.Fatalln("Could not open sqlite file: ", err)
	}

	_, err = o.db.Exec(`create table if not exists packets (
id integer not null, stream integer not null, num integer null,
enqueue integer not null, eligibility integer not null, transmission integer not null, hw integer not null, 
delta_t_e integer not null, delta_hw_e integer not null, arrival text not null,
send_flags integer not null, enq_index integer not null, num_spins integer not null, size integer not null,
PRIMARY KEY (id, stream)
);`)
	if err != nil {
		log.Fatalln("Could not create sqlite table: ", err)
	}
	o.AddNumbers()
	_, err = o.db.Exec("create index if not exists nums on packets (stream, num);")
	if err != nil {
		log.Fatalln("Could not create num index:", err)
	}

	_, err = o.db.Exec("create table if not exists speeds (start_time_ms integer not null primary key, num_bytes integer not null, num_bits integer null, speed real null);")
	if err != nil {
		log.Fatalln("Could not create sqlite speed table: ", err)
	}
	return o
}

func (o *SqliteOutput) ensureTx() {
	if o.tx == nil {
		var err error
		o.tx, err = o.db.Begin()
		if err != nil {
			log.Fatalln("Could not start sqlite transaction: ", err)
		}
		o.stmt, err = o.tx.Prepare("insert into packets values (?, ?, NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);")
		if err != nil {
			log.Fatalln("Could not prepare insert query: ", err)
		}
		o.speedStmt, err = o.tx.Prepare("insert into speeds values (?, ?, ?, ?);")
		if err != nil {
			log.Fatalln("Could not prepare insert query: ", err)
		}
	}
}

func (o *SqliteOutput) WritePacket(p *PacketTimings) {
	o.ensureTx()
	var err error
	_, err = o.stmt.Exec(p.I, p.Stream, p.Enqueue, p.Eligibility, p.Transmission, p.HwTransmission, p.DeltaET(), p.DeltaEHw(), p.Arrival, p.SendFlags, p.EnqueueIndex, p.NumSpins, p.Size)
	if err != nil {
		log.Printf("WARNING: Could not insert packet %d of stream %d: %v", p.I, p.Stream, err)
	}
}

func (o *SqliteOutput) WriteSpeed(p *SpeedData) {
	o.ensureTx()
	_, err := o.speedStmt.Exec(p.IntervalStart.UnixMilli(), p.NumBytes, p.NumBits, p.AvgSpeed)
	if err != nil {
		log.Println("WARNING: Could not insert speed: ", err)
	}
}

func (o *SqliteOutput) AddNumbers() {
	tx, err := o.db.Begin()
	if err != nil {
		log.Fatalln("Could not start transation for numbering:", err)
	}

	tbl := tx.QueryRow("select name, sql from sqlite_schema where type='table' and name='packets';")
	var table, cmd string
	err = tbl.Scan(&table, &cmd)
	if err != nil {
		tx.Rollback()
		log.Fatalln("Could not read table packets: ", err)
	}
	if !strings.Contains(strings.ToLower(cmd), "num integer null") {
		log.Println("Adding num column to existing table")
		_, err = tx.Exec("alter table packets add column num integer null;")
		if err != nil {
			tx.Rollback()
			log.Fatalln("Could not add num col to table:", err)
		}
	}

	var nullNums int
	nullNumsRow := tx.QueryRow("select count(*) from packets where num is null;")
	err = nullNumsRow.Scan(&nullNums)
	if err != nil {
		tx.Rollback()
		log.Fatalln("Could not query rows withou numbers:", err)
	}
	if nullNums > 0 {
		log.Println("Adding/Updating numbers to packets")

		nums, err := tx.Query("select id, stream, ROW_NUMBER() OVER (partition by stream ORDER BY id) as num from packets;")
		if err != nil {
			tx.Rollback()
			log.Fatalln("Could not add get numbered packets:", err)
		}
		for nums.Next() {
			var id, row uint64
			var stream uint16
			err = nums.Scan(&id, &stream, &row)
			if err != nil {
				tx.Rollback()
				log.Fatalln("could not get numbered row:", err)
			}
			_, err = tx.Exec("update packets set num=? where id=? and stream=?;", row, id, stream)
			if err != nil {
				log.Fatalf("Could not add number to packet %d/%d: %v", stream, id, err)
			}
		}
		err = nums.Err()
		if err != nil {
			tx.Rollback()
			log.Fatalln("Error while iterating over numbers:", err)
		}
	}
	err = tx.Commit()
	if err != nil {
		log.Fatalln("Error while commiting numbers:", err)
	}
}

func (o *SqliteOutput) Flush() {
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

func (o *SqliteOutput) Close() {
	o.Flush()
	if o.stmt != nil {
		o.stmt.Close()
	}
	o.db.Close()
}
