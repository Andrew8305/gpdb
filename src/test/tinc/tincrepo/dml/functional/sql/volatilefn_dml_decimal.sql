-- @author prabhd 
-- @created 2014-04-01 12:00:00
-- @modified 2012-04-01 12:00:00
-- @tags dml MPP-21090 ORCA
-- @product_version gpdb: [4.3-]
-- @optimizer_mode on	
-- @description Tests for MPP-21090
\echo --start_ignore
set gp_enable_column_oriented_table=on;
\echo --end_ignore
DROP TABLE IF EXISTS volatilefn_dml_decimal;
CREATE TABLE volatilefn_dml_decimal
(
    col1 decimal DEFAULT 1.00,
    col2 decimal DEFAULT 1.00,
    col3 int,
    col4 decimal DEFAULT 1.00
) 
DISTRIBUTED by (col1)
PARTITION BY LIST(col2)
(
default partition def 
);
DROP TABLE IF EXISTS volatilefn_dml_decimal_candidate;
CREATE TABLE volatilefn_dml_decimal_candidate
(
    col1 decimal DEFAULT 1.00,
    col2 decimal DEFAULT 1.00,
    col3 int,
    col4 decimal 
) 
DISTRIBUTED by (col2);
INSERT INTO volatilefn_dml_decimal_candidate(col3,col4) VALUES(10,2.00);

-- Create volatile UDF
CREATE OR REPLACE FUNCTION func(x int) RETURNS int AS $$
BEGIN

DROP TABLE IF EXISTS foo;
CREATE TABLE foo (a int, b int) distributed by (a);
INSERT INTO foo select i, i+1 from generate_series(1,10) i;

DROP TABLE IF EXISTS bar;
CREATE TABLE bar (c int, d int) distributed by (c);
INSERT INTO bar select i, i+1 from generate_series(1,10) i;

UPDATE bar SET d = d+1 WHERE c = $1;
RETURN $1 + 1;
END
$$ LANGUAGE plpgsql VOLATILE MODIFIES SQL DATA;

INSERT INTO volatilefn_dml_decimal(col1,col3) SELECT col2,func(14) FROM volatilefn_dml_decimal_candidate;
SELECT * FROM volatilefn_dml_decimal ORDER BY 1,2,3;

UPDATE volatilefn_dml_decimal SET col3 = (SELECT func(1));
SELECT * FROM volatilefn_dml_decimal ORDER BY 1,2,3;

DELETE FROM volatilefn_dml_decimal WHERE col3 = (SELECT func(1));
SELECT * FROM volatilefn_dml_decimal ORDER BY 1,2,3;

