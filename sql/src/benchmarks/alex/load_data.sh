#!/bin/sh
PWD=`pwd`
echo $PWD
SQL=sql_client
if [ $# -eq 1 ]; then
	SQL=$1
fi

#Generate Data...

#gcc -o earth earth.c
#./earth

#gcc -o tomo  tomo.c
#./tomo

#Bulk-Load into SQL server...

sql_client < create.sql
cat load.sql | sed -e s+PWD+$PWD+ | $SQL 

exit
#MapiClient <<EOF
#bulkload("earth", "$PWD/earth.tab", ",", "\n", 9600000);
#bulkload("tomo",  "$PWD/tomo.tab",  ",", "\n", 10092544);
#commit;
#quit;
#EOF

#Cleanup

rm earth.tab earth
rm tomo.tab tomo

