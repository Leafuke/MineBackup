#pragma once

#include "TaskCoordinator.h"

#include <vector>

class ApplicationEventRouter {
public:
	void Dispatch(const std::vector<TaskEvent>& events) const;

private:
	void DispatchOne(const TaskEvent& event) const;
};
