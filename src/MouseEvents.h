#pragma once

#include <chrono>

struct MouseEvents{
	float wheel_x = 0;
	float wheel_y = 0;
	float pos_x = 0;
	float pos_y = 0;
	int button = 0;
	int action = 0;
	int mods = 0;

	bool buttonPressed = false;
	bool mouseMoved = false;
	bool mouseScrolled = false;

	bool isLeftDown = false;
	bool isMiddleDown = false;
	bool isRightDown = false;
	bool hasEvent = false;

	// Double-click: two presses of the left button, at most 0.3s and 4 pixels apart. 
	// doubleClicked is set for the frame of the second press.
	bool doubleClicked = false;
	double lastLeftPressTime = -1.0;
	float lastLeftPress_x = 0;
	float lastLeftPress_y = 0;

	static double seconds(){
		auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
		return std::chrono::duration<double>(sinceEpoch).count();
	}


	void onMouseButton(int button, int action, int mods){

		this->button = this->button | button;
		this->action = this->action | action;
		this->mods   = this->mods | mods;

		if(button == 0 && action == 1){
			isLeftDown = true;

			double time = seconds();
			float dx = pos_x - lastLeftPress_x;
			float dy = pos_y - lastLeftPress_y;
			bool isSecondClick = time - lastLeftPressTime <= 0.3 && dx * dx + dy * dy <= 4.0f * 4.0f;

			if(isSecondClick){
				doubleClicked = true;
				lastLeftPressTime = -1.0; // a third click starts a new double-click
			}else{
				lastLeftPressTime = time;
				lastLeftPress_x = pos_x;
				lastLeftPress_y = pos_y;
			}
		}else if(action == 0){
			isLeftDown = false;
		}

		if(button == 2 && action == 1){
			isMiddleDown = true;
		}else if(action == 0){
			isMiddleDown = false;
		}

		if(button == 1 && action == 1){
			isRightDown = true;
		}else if(action == 0){
			isRightDown = false;
		}

		buttonPressed = true;
	}

	void onMouseMove(double xpos, double ypos){

		this->pos_x = xpos;
		this->pos_y = ypos;

		mouseMoved = true;
	}

	void onMouseScroll(double xoffset, double yoffset){

		this->wheel_x += xoffset;
		this->wheel_y += yoffset;

		mouseScrolled = true;
	}
	
	bool isLeftDownEvent(){
		return buttonPressed && button == 0 && action == 1;
	}

	bool isLeftUpEvent(){
		return buttonPressed && button == 0 && action == 0;
	}

	bool isRightDownEvent(){
		return buttonPressed && button == 1 && action == 1;
	}

	bool isRightUpEvent(){
		return buttonPressed && button == 1 && action == 0;
	}

	void clear(){
		wheel_x = 0;
		wheel_y = 0;
		button = 0;
		action = 0;
		mods = 0;

		buttonPressed = false;
		mouseMoved = false;
		mouseScrolled = false;
		doubleClicked = false;
	}
};