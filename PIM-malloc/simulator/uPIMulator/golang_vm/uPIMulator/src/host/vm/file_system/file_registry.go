package file_system

import "errors"

type FileRegistry struct {
	fbrk             int
	file_descriptors map[int]*FileDescriptor
}

func (this *FileRegistry) Init() {
	this.fbrk = 0
	this.file_descriptors = make(map[int]*FileDescriptor)
}

func (this *FileRegistry) NewFileDescriptor(path string) int {
	file_descriptor := new(FileDescriptor)
	file_descriptor.Init(path, this.fbrk)

	this.file_descriptors[this.fbrk] = file_descriptor

	fp := this.fbrk
	this.fbrk++
	return fp
}

func (this *FileRegistry) FindFileDescriptor(fp int) *FileDescriptor {
	if file_descriptor, found := this.file_descriptors[fp]; found {
		return file_descriptor
	} else {
		err := errors.New("file descriptor is not found")
		panic(err)
	}
}
