To compile please build the "YATE" project - it will build the library and all modules without external dependencies. The "Extra" project holds modules that have external dependencies. You will not be able to build them without installing extra headers and libraries in your Visual Studio environment:

h323chan needs PWLib and OpenH323;
gsmcodec needs a GSM 06.10 static library;
wpchan needs a VC++ compatible version of libpri;
Gtk2Client needs GTK and all its dependencies;
Qt4Client needs Qt® and its tools (MOC);
mysqldb needs MySQL client headers and libraries;
pgsqldb needs PostgreSQL client headers and libraries.

If you build Yate from sources and Visual Studio crashes or hangs beyond cancellation please clean the build and repeat from scratch. Already compiled object files may be good or may be damaged - just stay on the safe side.