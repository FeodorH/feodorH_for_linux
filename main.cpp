#include <iostream>
#include <string>
#include <fstream>

int main() {
  /// Flush after every std::cout / std:cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::cout << "$ ";
    std::string input;

    std::cin>>input;

    std::cout<<input<<" :)\n";
    
    return 0;
}
