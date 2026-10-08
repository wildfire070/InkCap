#include "Ao3StoreMaintenance.h"

#include "Ao3MarkedForLaterStore.h"
#include "Ao3NewChaptersStore.h"

void selfHealAo3PathStores() {
  if (AO3_MARKED_FOR_LATER_STORE.pruneMissing()) AO3_MARKED_FOR_LATER_STORE.saveToFile();
  if (AO3_NEW_CHAPTERS_STORE.pruneMissing()) AO3_NEW_CHAPTERS_STORE.saveToFile();
}
