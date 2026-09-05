#ifndef PVM_HEXUTIL_H_
#define PVM_HEXUTIL_H_

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

/* "AA BB CC"-style hex dump of `count` bytes starting at `offset`. */
inline std::string BytesToHex(const std::vector<uint8_t>& bytes, size_t offset, size_t count){
	std::ostringstream out;
	for(size_t i = 0; i < count; i++){
		if(i) out << ' ';
		out << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
				<< static_cast<unsigned>(bytes[offset + i]);
	}
	return out.str();
}

#endif /* PVM_HEXUTIL_H_ */
