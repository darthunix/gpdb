--
-- TIMETZ
--

CREATE TABLE TIMETZ_TBL (f1 time(2) with time zone);

INSERT INTO TIMETZ_TBL VALUES ('00:01 PDT');
INSERT INTO TIMETZ_TBL VALUES ('01:00 PDT');
INSERT INTO TIMETZ_TBL VALUES ('02:03 PDT');
INSERT INTO TIMETZ_TBL VALUES ('07:07 PST');
INSERT INTO TIMETZ_TBL VALUES ('08:08 EDT');
INSERT INTO TIMETZ_TBL VALUES ('11:59 PDT');
INSERT INTO TIMETZ_TBL VALUES ('12:00 PDT');
INSERT INTO TIMETZ_TBL VALUES ('12:01 PDT');
INSERT INTO TIMETZ_TBL VALUES ('23:59 PDT');
INSERT INTO TIMETZ_TBL VALUES ('11:59:59.99 PM PDT');

INSERT INTO TIMETZ_TBL VALUES ('2003-03-07 15:36:39 America/New_York');
INSERT INTO TIMETZ_TBL VALUES ('2003-07-07 15:36:39 America/New_York');
-- this should fail (the timezone offset is not known)
INSERT INTO TIMETZ_TBL VALUES ('15:36:39 America/New_York');

SELECT f1 AS "Time TZ" FROM TIMETZ_TBL ORDER BY 1;

SELECT f1 AS "Three" FROM TIMETZ_TBL WHERE f1 < '05:06:07-07' ORDER BY 1;

SELECT f1 AS "Seven" FROM TIMETZ_TBL WHERE f1 > '05:06:07-07' ORDER BY 1;

SELECT f1 AS "None" FROM TIMETZ_TBL WHERE f1 < '00:00-07' ORDER BY 1;

SELECT f1 AS "Ten" FROM TIMETZ_TBL WHERE f1 >= '00:00-07' ORDER BY 1;

--
-- TIME simple math
--
-- We now make a distinction between time and intervals,
-- and adding two times together makes no sense at all.
-- Leave in one query to show that it is rejected,
-- and do the rest of the testing in horology.sql
-- where we do mixed-type arithmetic. - thomas 2000-12-02

SELECT f1 + time with time zone '00:01' AS "Illegal" FROM TIMETZ_TBL;

-- basic sanity of sample timezones
set timezone = 'America/New_York';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as NYC;
set timezone = 'America/Los_Angeles';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as LA;
set timezone = 'Asia/Tokyo';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as Tokyo;
set timezone = 'Asia/Kolkata';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as Kolkata;
set timezone = 'Asia/Shanghai';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as Shanghai;
-- Casablanca time has changed in new timezone db lets verify it
set timezone = 'Africa/Casablanca';
SELECT TIMESTAMP WITH TIME ZONE 'epoch' + 1407545520 * INTERVAL '1 second' as Casablanca;
