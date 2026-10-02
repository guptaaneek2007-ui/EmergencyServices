#include <stdio.h>
#include <mysql.h>

int main() {
    MYSQL *conn = mysql_init(NULL);

    if (conn == NULL) {
        printf("MySQL initialization failed!\n");
        return 1;
    }

    if (mysql_real_connect(conn, "localhost", "root", "Aneek@28",
                           "Emergency_System", 3306, NULL, 0) == NULL) {
        printf("Connection failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    printf("MySQL connected successfully!\n");
    mysql_query(conn, "SELECT * FROM responders");

MYSQL_RES *result = mysql_store_result(conn);

if (result != NULL)
    printf("Responders table connected successfully!\n");
else
    printf("Table access failed!\n");
    mysql_close(conn);
    return 0;
}