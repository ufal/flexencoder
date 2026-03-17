// Local version of stoi - relies currently on C++ 11
int intval(std::string str);
bool preg_match ( std::string str, std::string pat, std::vector<std::string> *regmatch );
bool preg_match ( std::string str, std::string pat, std::string flags = "" );
std::vector<std::vector<std::string> > preg_match_all ( std::string str, std::string pat, std::string flags = "" );
std::string preg_replace ( std::string str, std::string pat, std::string to, std::string flags = "" );
std::string str2lower ( std::string str );
std::string trim ( std::string str );
std::string replace_all ( std::string str, std::string from, std::string to );
std::vector<std::string> split ( std::string str, std::string sep );
std::string float2string (float f);
std::string int2string (int f);
std::string join (std::vector<std::string> a, std::string b="." );
