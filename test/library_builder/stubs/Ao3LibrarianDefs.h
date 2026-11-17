#pragma once

#include <vector>

#include "../../../src/Ao3CompactIndexRecord.h"

// See Ao3LibrarianDefs.cpp for why this exists. Tests include this to inspect what
// LibraryBuilder's AO3 piggyback-write logic would have written.
namespace Ao3LibrarianDefs {
extern std::vector<CompactIndexRecord> writtenRecords;
}
