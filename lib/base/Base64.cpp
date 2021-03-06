/* <x0/Base64.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2011 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/Base64.h>

namespace x0 {

const char Base64::base64_[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789+/";

/* aaaack but it's fast and const should make it shared text page. */
const unsigned char Base64::pr2six_[256] = {
	/* ASCII table */
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
	64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
	64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

int Base64::encodeLength(int sourceLength) {
	return ((sourceLength + 2) / 3 * 4) + 1;
}

std::string Base64::encode(const std::string& text) {
	return encode((const unsigned char *)text.data(), text.size());
}

std::string Base64::encode(const Buffer& buffer) {
	return encode((const unsigned char *)buffer.data(), buffer.size());
}

std::string Base64::encode(const unsigned char *buffer, int ALength) {
	const int len = ALength;
	const unsigned char *string = buffer;

	char *encoded = new char[encodeLength(ALength) + 1];

	int i;
	char *p = encoded;
	for (i = 0; i < len - 2; i += 3) {
		*p++ = base64_[(string[i] >> 2) & 0x3F];
		*p++ = base64_[((string[i] & 0x3) << 4) | ((int) (string[i + 1] & 0xF0) >> 4)];
		*p++ = base64_[((string[i + 1] & 0xF) << 2) | ((int) (string[i + 2] & 0xC0) >> 6)];
		*p++ = base64_[string[i + 2] & 0x3F];
	}
	if (i < len) {
		*p++ = base64_[(string[i] >> 2) & 0x3F];
		if (i == (len - 1)) {
			*p++ = base64_[((string[i] & 0x3) << 4)];
			*p++ = '=';
		}
		else {
			*p++ = base64_[((string[i] & 0x3) << 4) | ((int) (string[i + 1] & 0xF0) >> 4)];
			*p++ = base64_[((string[i + 1] & 0xF) << 2)];
		}
		*p++ = '=';
	}
	//*p++ = '\0';
	int outlen = p - encoded;
	return std::string(encoded, 0, outlen);
}

int Base64::decodeLength(const std::string& buffer) {
	return decodeLength(buffer.c_str());
}

int Base64::decodeLength(const char *buffer) {
	register const unsigned char *bufin = (const unsigned char *) buffer;

	while (pr2six_[*(bufin++)] <= 63)
		;

	const int nprbytes = (bufin - (const unsigned char *)buffer) - 1;
	int nbytesdecoded = ((nprbytes + 3) / 4) * 3;

	return nbytesdecoded + 1;
}

Buffer Base64::decode(const std::string& value) {
	Buffer result;
	result.reserve(decodeLength(value));

	int len = decode(value.c_str(), (unsigned char *)result.data());

	result.resize(len);

	return result;
}

int Base64::decode(const char *input, unsigned char *output) {
	const char *bufcoded = input;
	unsigned char *bufplain = output;

	int nbytesdecoded;
	register const unsigned char *bufin;
	register unsigned char *bufout;
	register int nprbytes;

	bufin = (const unsigned char *) bufcoded;
	while (pr2six_[*(bufin++)] <= 63)
		;
	nprbytes = (bufin - (const unsigned char *) bufcoded) - 1;
	nbytesdecoded = (((int)nprbytes + 3) / 4) * 3;

	bufout = (unsigned char *) bufplain;
	bufin = (const unsigned char *) bufcoded;

	while (nprbytes > 4) {
		*(bufout++) = (unsigned char) (pr2six_[*bufin] << 2 | pr2six_[bufin[1]] >> 4);
		*(bufout++) = (unsigned char) (pr2six_[bufin[1]] << 4 | pr2six_[bufin[2]] >> 2);
		*(bufout++) = (unsigned char) (pr2six_[bufin[2]] << 6 | pr2six_[bufin[3]]);
		bufin += 4;
		nprbytes -= 4;
	}

	/* Note: (nprbytes == 1) would be an error, so just ingore that case */
	if (nprbytes > 1) {
		*(bufout++) =
		(unsigned char) (pr2six_[*bufin] << 2 | pr2six_[bufin[1]] >> 4);
	}
	if (nprbytes > 2) {
		*(bufout++) =
		(unsigned char) (pr2six_[bufin[1]] << 4 | pr2six_[bufin[2]] >> 2);
	}
	if (nprbytes > 3) {
		*(bufout++) =
		(unsigned char) (pr2six_[bufin[2]] << 6 | pr2six_[bufin[3]]);
	}

	nbytesdecoded -= (4 - (int)nprbytes) & 3;

	return nbytesdecoded;
}

} // namespace x0
