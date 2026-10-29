package expr

import (
	"errors"
	"uPIMulator/src/device/linker/lexer"
)

type ROpCodeExpr struct {
	token *lexer.Token
}

func (this *ROpCodeExpr) Init(token *lexer.Token) {
	token_type := token.TokenType()

	if token_type != lexer.TIME &&
		token_type != lexer.NOP &&
		token_type != lexer.INIT_LC {
		err := errors.New("token type is not an R op code")
		panic(err)
	}

	this.token = token
}

func (this *ROpCodeExpr) Token() *lexer.Token {
	return this.token
}
