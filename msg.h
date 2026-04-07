#include <stdint.h>

enum MsgType {
	MSG_GET_COLOR,
	MSG_SET_COLOR,
};

union MsgData {
	uint64_t u64;
	struct ColorRGBA color;
};

struct Msg {
	uint64_t id;
	union MsgData data;
};
