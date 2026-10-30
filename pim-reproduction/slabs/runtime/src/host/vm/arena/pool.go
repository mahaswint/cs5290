package arena

import (
	"errors"
	"fmt"
	"uPIMulator/src/encoding"
	"uPIMulator/src/host/vm/base"
	"uPIMulator/src/misc"
)

type Pool struct {
	offset  int64
	objects map[int64]*base.Object
	memory  *Memory

	last_object_address int64
}

func (this *Pool) Init() {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	this.offset = config_loader.VmBankOffset()

	this.objects = make(map[int64]*base.Object)

	this.memory = new(Memory)
	this.memory.Init(config_loader.VmMemorySize())

	this.last_object_address = 512
}

func (this *Pool) Memory() *Memory {
	return this.memory
}

func (this *Pool) Alloc(object_type base.ObjectType, size int64) *base.Object {
	address := this.last_object_address
	this.last_object_address += size

	object := new(base.Object)
	object.Init(object_type, address, size)
	this.objects[object.Address()] = object

	this.DoZeros(object)

	return object
}

func (this *Pool) Free(address int64) {
	if !this.HasObject(address) {
		err_msg := fmt.Sprintf("object with address (%d) is not found", address)
		err := errors.New(err_msg)
		panic(err)
	}

	delete(this.objects, address)
}

func (this *Pool) HasObject(address int64) bool {
	if _, found := this.objects[address]; found {
		return true
	} else {
		return false
	}
}

func (this *Pool) Object(address int64) *base.Object {
	if !this.HasObject(address) {
		err_msg := fmt.Sprintf("object with address (%d) is not found", address)
		err := errors.New(err_msg)
		panic(err)
	}

	if object, found := this.objects[address]; found {
		return object
	} else {
		return nil
	}
}

func (this *Pool) Objects() map[int64]*base.Object {
	return this.objects
}

func (this *Pool) DoZeros(object *base.Object) {
	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for i := int64(0); i < object.Size(); i++ {
		byte_stream.Append(0)
	}

	this.memory.Write(object.Address(), object.Size(), byte_stream)
}
