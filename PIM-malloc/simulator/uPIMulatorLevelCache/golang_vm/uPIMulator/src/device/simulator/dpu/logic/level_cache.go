package logic

import (
	"errors"
	"fmt"
	"uPIMulator/src/device/simulator/dpu/dram"
	"uPIMulator/src/encoding"
	"uPIMulator/src/misc"
)

type LevelCacheState int

const (
	LOOKUPABLE LevelCacheState = iota
	NON_LOOKUPABLE
)

type LevelCache struct {
	channel_id int
	rank_id    int
	dpu_id     int

	state      LevelCacheState
	cache      map[int64]int64
	lru        map[int64]int64
	cache_size int64
	cycle      int64

	mram_metadata_base int64
	dma                *Dma

	stat_factory *misc.StatFactory
}

func (this *LevelCache) Init(channel_id int, rank_id int, dpu_id int, command_line_parser *misc.CommandLineParser) {
	if channel_id < 0 {
		err := errors.New("channel ID < 0")
		panic(err)
	} else if rank_id < 0 {
		err := errors.New("rank ID < 0")
		panic(err)
	} else if dpu_id < 0 {
		err := errors.New("DPU ID < 0")
		panic(err)
	}

	this.channel_id = channel_id
	this.rank_id = rank_id
	this.dpu_id = dpu_id

	this.state = LOOKUPABLE
	this.cache = make(map[int64]int64)
	this.lru = make(map[int64]int64)
	this.cache_size = command_line_parser.IntParameter("level_cache_size")
	this.cycle = 0

	this.mram_metadata_base = 0
	this.dma = nil

	name := fmt.Sprintf("LevelCache%d_%d_%d", channel_id, rank_id, dpu_id)
	this.stat_factory = new(misc.StatFactory)
	this.stat_factory.Init(name)
}

func (this *LevelCache) Fini() {
}

func (this *LevelCache) StatFactory() *misc.StatFactory {
	return this.stat_factory
}

func (this *LevelCache) State() LevelCacheState {
	return this.state
}

func (this *LevelCache) InitMramMetadataBase(mram_metadata_base int64) {
	if this.mram_metadata_base != 0 {
		err := errors.New("MRAM metadata base is already set")
		panic(err)
	}

	this.mram_metadata_base = mram_metadata_base
}

func (this *LevelCache) ConnectDma(dma *Dma) {
	if this.dma != nil {
		err := errors.New("DMA is already set")
		panic(err)
	}

	this.dma = dma
}

func (this *LevelCache) Lookup(metadata_index int64) int64 {
	this.stat_factory.Increment("level_cache_access", 1)

	if _, found := this.cache[metadata_index]; found {
		this.stat_factory.Increment("level_cache_hit", 1)
		return 1
	} else {
		return 0
	}
}

func (this *LevelCache) Read(metadata_index int64) int64 {
	this.stat_factory.Increment("level_cache_read", 1)

	if metadata, found := this.cache[metadata_index]; found {
		this.lru[metadata_index] = this.cycle
		return metadata
	} else {
		err := errors.New("metadata is not found")
		panic(err)
	}
}

func (this *LevelCache) Write(metadata_index int64, metadata int64) {
	this.stat_factory.Increment("level_cache_write", 1)

	for this.Size() >= this.cache_size {
		this.Flush()
	}

	this.cache[metadata_index] = metadata
	this.lru[metadata_index] = this.cycle
}

func (this *LevelCache) Flush() {
	this.stat_factory.Increment("level_cache_flush", 1)

	lru_metadata_index := int64(-1)
	lru_cycle := int64(-1)

	for metadata_index, cycle := range this.lru {
		if lru_cycle == -1 {
			lru_cycle = cycle
			lru_metadata_index = metadata_index
		} else if cycle < lru_cycle {
			lru_cycle = cycle
			lru_metadata_index = metadata_index
		}
	}

	mram_address := this.mram_metadata_base + lru_metadata_index*4
	metadata := this.cache[lru_metadata_index]

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()
	byte_stream.Append(uint8(metadata & 0xFF))
	byte_stream.Append(uint8((metadata >> 8) & 0xFF))
	byte_stream.Append(uint8((metadata >> 16) & 0xFF))
	byte_stream.Append(uint8((metadata >> 24) & 0xFF))

	dma_command := new(dram.DmaCommand)
	dma_command.InitWriteToMram(mram_address, 4, byte_stream)

	this.dma.Push(dma_command)
	this.state = NON_LOOKUPABLE

	delete(this.cache, lru_metadata_index)
	delete(this.lru, lru_metadata_index)
}

func (this *LevelCache) Size() int64 {
	return int64(len(this.cache) * 4)
}

func (this *LevelCache) Cycle() {
	this.ServiceDma()

	this.cycle++
}

func (this *LevelCache) ServiceDma() {
	if this.dma.CanPop() {
		dma_command := this.dma.Front()

		if dma_command.Instruction() == nil {
			if this.state != NON_LOOKUPABLE {
				err := errors.New("level cache's state is not block")
				panic(err)
			}

			this.state = LOOKUPABLE
			this.dma.Pop()
		}
	}
}
