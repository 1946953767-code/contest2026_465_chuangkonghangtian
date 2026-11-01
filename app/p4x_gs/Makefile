############################################################################
# apps/examples/p4x_gs/Makefile
#
# P4X rocket telemetry ground station UI (Team465 创空航天)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
############################################################################

include $(APPDIR)/Make.defs

# P4X ground station UI

PROGNAME  = $(CONFIG_EXAMPLES_P4X_GS_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_P4X_GS_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_P4X_GS_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_P4X_GS)

MAINSRC = p4x_gs_main.c

include $(APPDIR)/Application.mk
