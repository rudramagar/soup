#ifndef RUN_MODES_H
#define RUN_MODES_H

#include "app_args.h"
#include "filter.h"

int run_itch(const AppArgs& args, Filter& filter);
int run_glimpse(const AppArgs& args, Filter& filter);
int run_ouch(const AppArgs& args, Filter& filter);

#endif
