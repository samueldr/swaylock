#ifndef _PINPAD_H
#define _PINPAD_H

enum pad_button {
	PAD_BUTTON_INVALID = -1,
	// BUTTON_ZERO is the actual button in the grid.
	PAD_BUTTON_0  =  0,
	PAD_BUTTON_1  =  1,
	PAD_BUTTON_2  =  2,
	PAD_BUTTON_3  =  3,
	PAD_BUTTON_4  =  4,
	PAD_BUTTON_5  =  5,
	PAD_BUTTON_6  =  6,
	PAD_BUTTON_7  =  7,
	PAD_BUTTON_8  =  8,
	PAD_BUTTON_9  =  9,
	PAD_BUTTON_BACKSPACE = 10,
	PAD_BUTTON_ZERO = 11,
	PAD_BUTTON_SEND = 12,
};

//enum pad_button pinpad_button_at(int x, int y);

#endif
