#/bin/bash

# STC1000+, improved firmware and Arduino based firmware uploader for the STC-1000 dual stage thermostat.
#
# Copyright 2014 Mats Staffansson
#
# This file is part of STC1000+.
#
# STC1000+ is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# STC1000+ is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with STC1000+.  If not, see <http://www.gnu.org/licenses/>.

# This is a simple script to make building STC-1000+ releases easier.

# Build HEX files
make clean all

# Extract version info from stc1000p.h
v=`cat stc1000p.h | grep STC1000P_VERSION`
e=`cat stc1000p.h | grep STC1000P_EEPROM_VERSION`

# Remove embedded HEX data from previous sketch and insert version info
cat ../picprog.ino | sed -n '/^const char hex_celsius\[\] PROGMEM/q;p' | sed "s/^#define STC1000P_VERSION.*/$v/" | sed "s/^#define STC1000P_EEPROM_VERSION.*/$e/" >> picprog.tmp

# Insert new HEX data
echo "const char hex_celsius[] PROGMEM = {" >> picprog.tmp; 
for l in `cat build/stc1000p_celsius.hex | sed 's/^://' | sed 's/\(..\)/0\x\1\,/g'`; do 
	echo "   $l" | sed 's/0x00,0x00,0x00,0x01,0xFF,/0x00,0x00,0x00,0x01,0xFF/' >> picprog.tmp; 
done; 
echo "};" >> picprog.tmp

echo "const char hex_fahrenheit[] PROGMEM = {" >> picprog.tmp; 
for l in `cat build/stc1000p_fahrenheit.hex | sed 's/^://' | sed 's/\(..\)/0\x\1\,/g'`; do 
	echo "   $l" | sed 's/0x00,0x00,0x00,0x01,0xFF,/0x00,0x00,0x00,0x01,0xFF/' >> picprog.tmp; 
done; 
echo "};" >> picprog.tmp

echo "const char hex_eeprom_celsius[] PROGMEM = {" >> picprog.tmp; 
for l in `cat build/eedata_celsius.hex | sed 's/^://' | sed 's/\(..\)/0\x\1\,/g'`; do 
	echo "   $l" | sed 's/0x00,0x00,0x00,0x01,0xFF,/0x00,0x00,0x00,0x01,0xFF/' >> picprog.tmp; 
done; 
echo "};" >> picprog.tmp

echo "const char hex_eeprom_fahrenheit[] PROGMEM = {" >> picprog.tmp; 
for l in `cat build/eedata_fahrenheit.hex | sed 's/^://' | sed 's/\(..\)/0\x\1\,/g'`; do 
	echo "   $l" | sed 's/0x00,0x00,0x00,0x01,0xFF,/0x00,0x00,0x00,0x01,0xFF/' >> picprog.tmp; 
done; 
echo "};" >> picprog.tmp

# Create picprog.js
cat picprog.tmp | sed -n '/^const char hex_eeprom_celsius\[\] PROGMEM/q;p' | sed "s/'/\\\\\\\'/g" > picprog.js.tmp
echo "var sketch='' +" > ../profile/picprog.js
while IFS= read r; do
	echo "'$r\n' +"; 
done < picprog.js.tmp >> ../profile/picprog.js
echo "'';" >> ../profile/picprog.js
rm -f picprog.js.tmp

# Rename old sketch and replace with new
mv -f ../picprog.ino picprog.bkp
mv picprog.tmp ../picprog.ino

# Print size approximation (from .asm files)
let s=0;
for a in `cat build/page0_c.asm build/page1_c.asm | grep instructions | sed 's/^.*=  //' | sed 's/ instructions.*//'`;
do
	echo $a;
	s=$(($s+$a));
done;
echo "total $s";

make clean

