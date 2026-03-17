// BOOST version of the local functions (regex, stoi)
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <sstream>  
#include <stdio.h>
#include <fstream>
#include <vector>
#include <boost/regex.hpp>
#include <boost/lexical_cast.hpp>

// Local version of stoi - relies currently on C++ 11
int intval(std::string str) {
	int i;
	
	try {
		i = boost::lexical_cast<int>(str);
	} catch (...) {
		std::cout << "Failed to convert to integer: " << str << std::endl; 
		return -1;
	};
	
	return i;
};

std::string float2string ( float num ) {
	return boost::lexical_cast<std::string>(num);
};

std::string int2string ( int num ) {
	return boost::lexical_cast<std::string>(num);
};

// join a vector of strings into a string
std::string join (std::vector<std::string> elems, std::string delim="." ) {
	std::string joinstr = "";
	
	joinstr = boost::algorithm::join(elems, delim);
	
	return joinstr;
};

// Local version of regex_match - relies currently on C++ 11 (could also do boost)
bool preg_match ( std::string str, std::string pat, std::vector<std::string> *regmatch ) {
	// Instead of regex_match, we could also iterate
	bool res = false;
	regmatch->clear();
	
	boost::regex e (pat);   // matches words beginning by "sub"

	boost::cmatch m;
	try {
		res = boost::regex_match (str.c_str(), m, e );
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

// 	boost::regex re;
// 	if ( flags.find("c") != std::string::npos ) {
// 		re = boost::regex(restr, boost::regex_constants::icase);
// 	} else {
// 		re = boost::regex(restr);
// 	};

	bool res = preg_match ( str, pat, &matches );
	
	return res;
};

std::vector<std::vector<std::string> > preg_match_all ( std::string str, std::string pat, std::string flags = "" ) {
	std::vector<std::vector<std::string> >  results;

    try {
        boost::regex exp(pat) ;

        boost::match_results<std::string::const_iterator> iter;

        std::string::const_iterator start = str.begin() ;
        std::string::const_iterator end = str.end() ;

        while ( boost::regex_search(start, end, iter, exp) )
        {
		std::vector<std::string> match;
			for (int i=0; i<iter.size(); i++ ) {
				match.push_back(iter[i]);
			};
			results.push_back(match);
            start = iter[0].second ;
        }
    } catch ( boost::bad_expression & ex ) {
        // std::cout << ex.what() ;
    }
    	
	return results;
};

std::string preg_replace ( std::string str, std::string pat, std::string to, std::string flags = "" ) {
	std::string result;

	boost::regex re(pat);
	result = boost::regex_replace(str, re, to);

	return result;
};

std::string str2lower(std::string str) {
	boost::to_lower(str);
	return str;
};
std::string trim(std::string str) {
	boost::trim(str);
	return str;
};
std::vector<std::string> split ( std::string str, std::string sep ) {
	std::vector<std::string> parts;

    iter_split(parts, str, boost::algorithm::first_finder(sep));
    	
	return parts;
};
std::string replace_all ( std::string str, std::string from, std::string to ) {
	boost::replace_all(str, from, to);
	return str;
};