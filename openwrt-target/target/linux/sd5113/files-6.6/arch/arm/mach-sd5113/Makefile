# SPDX-License-Identifier: GPL-2.0
#
# Makefile for arch/arm/mach-sd5113.
#
# STATUS: NEW in round 16 - this file was MISSING entirely from round
# 15's initial mach-sd5113/ directory (only board.c and Kconfig existed).
# Without it, board.c would never actually be compiled or linked in,
# silently - this would have looked identical to a working build right
# up until boot, where it would have failed exactly like round 15's
# original "no machine descriptor at all" problem, just one layer
# deeper and harder to notice. Found only once a real kernel source
# tree became available to attempt an actual build against (round 16).

obj-y	:= board.o
