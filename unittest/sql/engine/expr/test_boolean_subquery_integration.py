"""Issue #1477: boolean rewrites must preserve enclosing IFNULL types."""

import os
import unittest

import pymysql


class BooleanSubqueryIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connection = pymysql.connect(host=os.getenv("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.getenv("SEEKDB_TEST_PORT", "2881")), user="root", database="test",
            password=os.getenv("SEEKDB_TEST_PASSWORD", ""), autocommit=True, read_timeout=30)

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()

    def rows(self, sql):
        with self.connection.cursor() as cursor:
            cursor.execute(sql)
            return cursor.fetchall()

    def test_issue_and_controls(self):
        for sql in (
            'SELECT 1 FROM dual WHERE IFNULL((SELECT FALSE), !(!(1 LIKE "x")))',
            'SELECT 1 FROM dual WHERE IFNULL((SELECT FALSE), !(1 LIKE "x"))',
            'SELECT 1 FROM dual WHERE IFNULL(FALSE, !(!(1 LIKE "x")))',
        ):
            with self.subTest(sql=sql):
                self.assertEqual(self.rows(sql), ())

    def test_null_true_and_false_subqueries_preserve_three_valued_logic(self):
        for scalar, fallback in (("FALSE", 0), ("TRUE", 1), ("NULL", None)):
            for value, like in (("'x'", 1), ("'z'", 0), ("NULL", None)):
                for negations in (2, 4, 6):
                    sql = f"SELECT IFNULL((SELECT {scalar}), " + "!(" * negations + f"{value} LIKE 'x'" + ")" * negations + ")"
                    expected = like if fallback is None else fallback
                    with self.subTest(sql=sql):
                        self.assertEqual(self.rows(sql), ((expected,),))

    def test_correlated_subquery_and_non_boolean_values(self):
        self.assertEqual(self.rows("""
            SELECT t.id, IFNULL((SELECT t.v), !(!(t.word LIKE 'x')))
            FROM (SELECT 1 AS id, NULL AS v, 'x' AS word
                  UNION ALL SELECT 2, NULL, 'z'
                  UNION ALL SELECT 3, 7, 'z') AS t ORDER BY t.id
        """), ((1, 1), (2, 0), (3, 7)))
        self.assertEqual(self.rows("SELECT IFNULL((SELECT NULL), !(!(9)))"), ((1,),))


if __name__ == "__main__":
    unittest.main(verbosity=2)
