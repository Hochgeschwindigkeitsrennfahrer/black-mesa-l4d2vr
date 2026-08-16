#include <regex>
#include <iostream>
int main(){
    try {
        std::regex r("^(?:\\[[^\\]]+\\]\\s*)?(.+? ???[^.?]+[.?]?)$", std::regex_constants::ECMAScript);
        std::cout << "ok\n";
    } catch (const std::regex_error& e) {
        std::cout << "regex_error:" << e.code() << "\n";
    }
    return 0;
}
