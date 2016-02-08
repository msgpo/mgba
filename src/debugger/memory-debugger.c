/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "memory-debugger.h"

#include "debugger.h"

#include "util/math.h"

#include <string.h>

static bool _checkWatchpoints(struct Debugger* debugger, uint32_t address, struct DebuggerEntryInfo* info, enum WatchpointType type, uint32_t newValue, int width);

#define FIND_DEBUGGER(DEBUGGER, CPU) \
	{ \
		DEBUGGER = 0; \
		size_t i; \
		for (i = 0; i < CPU->numComponents; ++i) { \
			if (CPU->components[i]->id == DEBUGGER_ID) { \
				DEBUGGER = (struct Debugger*) cpu->components[i]; \
				break; \
			} \
		} \
	}

#define CREATE_SHIM(NAME, RETURN, TYPES, ...) \
	static RETURN DebuggerShim_ ## NAME TYPES { \
		struct Debugger* debugger; \
		FIND_DEBUGGER(debugger, cpu); \
		return debugger->originalMemory.NAME(cpu, __VA_ARGS__); \
	}

#define CREATE_WATCHPOINT_READ_SHIM(NAME, WIDTH, RETURN, TYPES, ...) \
	static RETURN DebuggerShim_ ## NAME TYPES { \
		struct Debugger* debugger; \
		FIND_DEBUGGER(debugger, cpu); \
		struct DebuggerEntryInfo info; \
		if (_checkWatchpoints(debugger, address, &info, WATCHPOINT_READ, 0, WIDTH)) { \
			DebuggerEnter(debugger, DEBUGGER_ENTER_WATCHPOINT, &info); \
		} \
		return debugger->originalMemory.NAME(cpu, __VA_ARGS__); \
	}

#define CREATE_WATCHPOINT_WRITE_SHIM(NAME, WIDTH, RETURN, TYPES, ...) \
	static RETURN DebuggerShim_ ## NAME TYPES { \
		struct Debugger* debugger; \
		FIND_DEBUGGER(debugger, cpu); \
		struct DebuggerEntryInfo info; \
		if (_checkWatchpoints(debugger, address, &info, WATCHPOINT_WRITE, value, WIDTH)) { \
			DebuggerEnter(debugger, DEBUGGER_ENTER_WATCHPOINT, &info); \
		} \
		return debugger->originalMemory.NAME(cpu, __VA_ARGS__); \
	}

#define CREATE_MULTIPLE_WATCHPOINT_SHIM(NAME, ACCESS_TYPE) \
	static uint32_t DebuggerShim_ ## NAME (struct ARMCore* cpu, uint32_t address, int mask, enum LSMDirection direction, int* cycleCounter) { \
		struct Debugger* debugger; \
		FIND_DEBUGGER(debugger, cpu); \
		uint32_t popcount = popcount32(mask); \
		int offset = 4; \
		int base = address; \
		if (direction & LSM_D) { \
			offset = -4; \
			base -= (popcount << 2) - 4; \
		} \
		if (direction & LSM_B) { \
			base += offset; \
		} \
		unsigned i; \
		for (i = 0; i < popcount; ++i) { \
			struct DebuggerEntryInfo info; \
			if (_checkWatchpoints(debugger, base + 4 * i, &info, ACCESS_TYPE, 0, 4)) { \
				DebuggerEnter(debugger, DEBUGGER_ENTER_WATCHPOINT, &info); \
			} \
		} \
		return debugger->originalMemory.NAME(cpu, address, mask, direction, cycleCounter); \
	}

CREATE_WATCHPOINT_READ_SHIM(load32, 4, uint32_t, (struct ARMCore* cpu, uint32_t address, int* cycleCounter), address, cycleCounter)
CREATE_WATCHPOINT_READ_SHIM(load16, 2, uint32_t, (struct ARMCore* cpu, uint32_t address, int* cycleCounter), address, cycleCounter)
CREATE_WATCHPOINT_READ_SHIM(load8, 1, uint32_t, (struct ARMCore* cpu, uint32_t address, int* cycleCounter), address, cycleCounter)
CREATE_WATCHPOINT_WRITE_SHIM(store32, 4, void, (struct ARMCore* cpu, uint32_t address, int32_t value, int* cycleCounter), address, value, cycleCounter)
CREATE_WATCHPOINT_WRITE_SHIM(store16, 2, void, (struct ARMCore* cpu, uint32_t address, int16_t value, int* cycleCounter), address, value, cycleCounter)
CREATE_WATCHPOINT_WRITE_SHIM(store8, 1, void, (struct ARMCore* cpu, uint32_t address, int8_t value, int* cycleCounter), address, value, cycleCounter)
CREATE_MULTIPLE_WATCHPOINT_SHIM(loadMultiple, WATCHPOINT_READ)
CREATE_MULTIPLE_WATCHPOINT_SHIM(storeMultiple, WATCHPOINT_WRITE)
CREATE_SHIM(setActiveRegion, void, (struct ARMCore* cpu, uint32_t address), address)

static bool _checkWatchpoints(struct Debugger* debugger, uint32_t address, struct DebuggerEntryInfo* info, enum WatchpointType type, uint32_t newValue, int width) {
	--width;
	struct DebugWatchpoint* watchpoint;
	size_t i;
	for (i = 0; i < DebugWatchpointListSize(&debugger->watchpoints); ++i) {
		watchpoint = DebugWatchpointListGetPointer(&debugger->watchpoints, i);
		if (!((watchpoint->address ^ address) & ~width) && watchpoint->type & type) {
			switch (width + 1) {
			case 1:
				info->oldValue = debugger->originalMemory.load8(debugger->cpu, address, 0);
				break;
			case 2:
				info->oldValue = debugger->originalMemory.load16(debugger->cpu, address, 0);
				break;
			case 4:
				info->oldValue = debugger->originalMemory.load32(debugger->cpu, address, 0);
				break;
			}
			info->newValue = newValue;
			info->address = address;
			info->watchType = watchpoint->type;
			info->accessType = type;
			return true;
		}
	}
	return false;
}

void DebuggerInstallMemoryShim(struct Debugger* debugger) {
	debugger->originalMemory = debugger->cpu->memory;
	debugger->cpu->memory.store32 = DebuggerShim_store32;
	debugger->cpu->memory.store16 = DebuggerShim_store16;
	debugger->cpu->memory.store8 = DebuggerShim_store8;
	debugger->cpu->memory.load32 = DebuggerShim_load32;
	debugger->cpu->memory.load16 = DebuggerShim_load16;
	debugger->cpu->memory.load8 = DebuggerShim_load8;
	debugger->cpu->memory.storeMultiple = DebuggerShim_storeMultiple;
	debugger->cpu->memory.loadMultiple = DebuggerShim_loadMultiple;
	debugger->cpu->memory.setActiveRegion = DebuggerShim_setActiveRegion;
}

void DebuggerRemoveMemoryShim(struct Debugger* debugger) {
	debugger->cpu->memory.store32 = debugger->originalMemory.store32;
	debugger->cpu->memory.store16 = debugger->originalMemory.store16;
	debugger->cpu->memory.store8 = debugger->originalMemory.store8;
	debugger->cpu->memory.load32 = debugger->originalMemory.load32;
	debugger->cpu->memory.load16 = debugger->originalMemory.load16;
	debugger->cpu->memory.load8 = debugger->originalMemory.load8;
	debugger->cpu->memory.storeMultiple = debugger->originalMemory.storeMultiple;
	debugger->cpu->memory.loadMultiple = debugger->originalMemory.loadMultiple;
	debugger->cpu->memory.setActiveRegion = debugger->originalMemory.setActiveRegion;
}
