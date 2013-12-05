Ganglia metric check for Nagios
===============================

A plugin written in C for Nagios to check Ganglia metrics. Inspired by the Python plugin [check\_ganglia\_metric](https://github.com/mconigliaro/check_ganglia_metric) by Michael Paul Thomas Conigliaro which is based on [check\_ganglia\_metric.php](https://github.com/ganglia/ganglia-web) by Vladimir Vuksan.

This plugin makes several improvements to previous projects to make a high volume of checks possible:

* gmetad result caching
* Checking host metric(s) or heartbeat using one plugin from cache
* Written in C to avoid the overhead of invoking an interpreter for each check
