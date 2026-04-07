#include <stdint.h>

enum MsgType {
	MSG_GET_COLOR,
	MSG_SET_COLOR,
};

struct Msg {
	uint64_t id;
	uint64_t data;
};
