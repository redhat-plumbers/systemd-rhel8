/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

char* setup_fake_runtime_dir(void);
bool test_is_running_from_builddir(char **exedir);
const char* get_testdata_dir(void);
void test_setup_logging(int level);
