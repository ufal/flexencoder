// C++ 11 (version > 4.9) version of the local functions (regex, stoi)
#include <iostream>
#include <vector>
#include <regex>
#include <algorithm> 
#include <cctype>
#include <locale>
#include <string>
#include <sstream>
#include <iterator>

// Local version of stoi - relies currently on C++ 11
int intval(std::string str) {
	int i;
	
	try {
		std::string::size_type sz;   // alias of size_t
		i = stoi(str,&sz,10);
	} catch (...) {
		std::cout << "Failed to convert to integer: " << str << std::endl; 
		return -1;
	};
	
	return i;
};

// Local version of regex_match - relies currently on C++ 11 (could also do boost)
bool preg_match ( std::string str, std::string pat, std::vector<std::string> *regmatch ) {
	// Instead of regex_match, we could also iterate
	bool res = false;
	regmatch->clear();
	
	std::cmatch m;
	try {
		res = std::regex_match (str.c_str(), m, std::regex(pat, std::regex_constants::extended) );
    } catch (const std::regex_error& e) {
		std::cout << "Error in the pattern: " << pat << std::endl; 
		return false;
    } catch (...) {
		std::cout << "Error in the pattern: " << pat << std::endl; 
		return false;
	};

	for ( int i=0; i<m.size(); i++ ) {
 	  	std::string mtch = m[i];
 		regmatch->push_back(mtch);
	};

	return res;
};
bool preg_match ( std::string str, std::string pat, std::string flags = "" ) { // variant without a vector
	std::vector<std::string> matches;

// TODO: implement flags
// 	std::regex re;
// 	if ( flags.find("c") != std::string::npos ) {
// 		re = std::regex(restr, std::regex_constants::icase);
// 	} else {
// 		re = std::regex(restr);
// 	};

	bool res = preg_match ( str, pat, &matches );
	
	return res;
};

std::string float2string ( float num ) {
	return std::to_string(num);
};

std::string int2string ( int num ) {
	return std::to_string(num);
};

std::vector<std::vector<std::string> > preg_match_all ( std::string str, std::string pat, std::string flags = "" ) {
	std::vector<std::vector<std::string> >  results;

	std::match_results<std::string::const_iterator> iter;
	std::string::const_iterator start = str.begin() ; int i=0;
	while ( regex_search(start, str.cend(), iter, std::regex(pat, std::regex_constants::icase)) ) {
		std::vector<std::string> match;
		for (int i=0; i<iter.size(); i++ ) {
			match.push_back(iter[i]);
		};
		results.push_back(match);
		start = iter[0].second ;i++;
	};
	
	return results;
};

std::string preg_replace ( std::string str, std::string pat, std::string to, std::string flags = "" ) {
	std::string result;
	std::regex e ( pat ); 
	std::regex_replace (std::back_inserter(result), str.begin(), str.end(), e, to);
	return result;
};

std::string str2lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), 
                   [](unsigned char c){ return std::tolower(c); } // correct
                  );
    return str;
};
// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// join a vector of strings into a string
std::string join (std::vector<std::string> elems, std::string b="." ) {
	
	const char* const delim = b.c_str();

	std::ostringstream imploded;
	std::copy(elems.begin(), elems.end(),
			   std::ostream_iterator<std::string>(imploded, delim));
           	
	return imploded.str();
};


// trim from both ends (in place)
static inline void itrim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

// trim from both ends (copying)
std::string trim (std::string s) {
    itrim(s);
    return s;
}
std::vector<std::string> split ( std::string str, std::string sep ) {
	std::vector<std::string> parts;

    std::size_t start = str.find_first_not_of(sep), end = 0;

    while((end = str.find_first_of(sep, start)) != std::string::npos)
    {
        parts.push_back(str.substr(start, end - start));
        start = str.find_first_not_of(sep, end);
    }
    if(start != std::string::npos)
        parts.push_back(str.substr(start));
	
	return parts;
};
std::string replace_all ( std::string str, std::string from, std::string to ) {
	size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }
    
    return str;
};