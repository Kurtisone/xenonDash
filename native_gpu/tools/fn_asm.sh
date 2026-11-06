#!/bin/sh
# usage: fn.sh ADDR  -> asm comments of function
f=$(grep -l "^DEFINE_REX_FUNC(sub_$1)" /home/kurtisone/Projects/xbox360/dashboards/2.0.6770.0/generated/dash/dash_recomp.*.cpp | head -1)
awk -v n="DEFINE_REX_FUNC(sub_$1)" '$0 ~ "^DEFINE_REX_FUNC" {on=(index($0,n)==1)} on && /^\t\/\// {print}' $f
