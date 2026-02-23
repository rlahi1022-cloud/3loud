#include <iostream>
#include <mariadb/conncpp.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    try {
        // 1. DB 연결 설정
        sql::Driver* driver = sql::mariadb::get_driver_instance();
        sql::SQLString url("jdbc:mariadb://localhost:3306/test");
        sql::Properties properties({{"user", "범준"}, {"password", "1234"}});
        
        std::unique_ptr<sql::Connection> conn(driver->connect(url, properties));

        // 2. 쿼리 실행
        std::unique_ptr<sql::Statement> stmnt(conn->createStatement());
        sql::ResultSet *res = stmnt->executeQuery("SELECT id, name FROM TASKS");

        // 3. 데이터를 JSON으로 변환
        json j_list = json::array();
        while (res->next()) {
            json item;
            item["id"] = res->getInt("id");
            item["name"] = res->getString("name").c_str();
            j_list.push_back(item);
        }

        // 4. 결과 출력
        std::cout << j_list.dump(4) << std::endl;

    } catch (sql::SQLException& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}