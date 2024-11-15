SELECT * FROM ( EXPLAIN AST CREATE TABLE test ENGINE=Memory ); -- {clientError BAD_ARGUMENTS}
SELECT * FROM ( EXPLAIN AST CREATE MATERIALIZED VIEW mv (data String) AS SELECT data FROM table ); -- {clientError BAD_ARGUMENTS}
SELECT * FROM ( EXPLAIN AST INSERT INTO TABLE test VALUES); -- {clientError BAD_ARGUMENTS}
SELECT * FROM ( EXPLAIN AST ALTER TABLE test MODIFY COLUMN x UInt32 ); -- {clientError BAD_ARGUMENTS}
