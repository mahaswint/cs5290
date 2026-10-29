package file_system

import (
	"strconv"
	"strings"
	"uPIMulator/src/misc"
)

type FileDescriptor struct {
	path  string
	fp    int
	words []int
	pos   int
}

func (this *FileDescriptor) Init(path string, fp int) {
	this.path = path
	this.fp = fp

	this.words = make([]int, 0)
	this.pos = 0

	file_scanner := new(misc.FileScanner)
	file_scanner.Init(path)

	for _, line := range file_scanner.ReadLines() {
		line = strings.Replace(line, "\t", " ", -1)

		words := strings.Split(line, " ")

		for _, word := range words {
			int_word, err := strconv.Atoi(word)
			if err != nil {
				panic(err)
			}

			this.words = append(this.words, int_word)
		}
	}
}

func (this *FileDescriptor) Scan() int {
	word := this.words[this.pos]
	this.pos++
	return word
}
