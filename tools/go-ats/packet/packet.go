package packet

import (
	"time"
)

type PacketTimings struct {
	Stream         uint16
	I              uint64
	Enqueue        time.Duration
	Eligibility    time.Duration
	Transmission   time.Duration
	HwTransmission time.Duration
	Arrival        time.Time
	SendFlags      uint8 // 0b1: isr; 0b10: more than 1 packet, 0x80: switch packet drop
	EnqueueIndex   uint8
	NumSpins       uint8
	Size           uint16
}

type SpeedData struct {
	IntervalStart time.Time
	NumBytes      uint64
	NumBits       uint64
	AvgSpeed      float64
}

func (t PacketTimings) DeltaET() time.Duration {
	return t.Transmission - t.Eligibility
}

func (t PacketTimings) DeltaEHw() time.Duration {
	if t.HwTransmission > 0 {
		return t.HwTransmission - t.Eligibility
	}
	return 0
}
