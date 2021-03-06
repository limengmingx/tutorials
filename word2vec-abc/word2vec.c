//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

// Modified by dolaameng for learning purpose 
// Modifications include (1) simplicfication (2) more comments

// Algorithm Discussion from the project page at https://code.google.com/p/word2vec/
// * architecture: skip-gram (slower, better for infrequent words) vs CBOW (fast)
// * the training algorithm: hierarchical softmax (better for infrequent words) vs negative sampling (better for frequent words, better with low dimensional vectors)
// * sub-sampling of frequent words: can improve both accuracy and speed for large data sets (useful values are in range 1e-3 to 1e-5)
// * dimensionality of the word vectors: usually more is better, but not always
// * context (window) size: for skip-gram usually around 10, for CBOW around 5

// The generated features from the words are stored in the syn0 structure, 
// the dimensionality of the feature space is layer1_size
// So the dth feature for the cth word in vocab is syn0[c * layer1_size + d]

// main datastructure 
// * struct vocab_word - structure for word in vocabulary
// * vocab - the collection of all vocab_words (size: vocab_max_size, vocab_size)
// * vocab_hash - the collection of integers (hash of words), (size: vocab_hash_size)
// *** the values in vocab_hash are the index of words in vocabulary
// * table - the coolection of integers (??)
// * model data structure
// * syn0 - collection of real values (features of words as flattend) 
// * syn1, syn1neg, 
// * expTable - exponetial table for speed-up of sigmoid calculation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000
#define MAX_CODE_LENGTH 40

// Maximum 30 * 0.7 = 21M words in the voc 
const int vocab_hash_size = 30000000; 

// Precision of float numbers
typedef float real;

struct vocab_word {
	long long cn; // word count, read from vocab file or counted from train
	int * point; // binary tree edges
	char *word, *code, codelen; // word: the string, code: binary tree code, codelen: depth (len of code)
};

char train_file[MAX_STRING], output_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];

// vocab table
struct vocab_word *vocab;
// flags: cbow = cbow architecture
int binary = 0, cbow = 0, debug_mode = 2;
int window = 5, min_count = 5; /*min counts for word from vocab to stay in vocab*/ 
int num_threads = 1, min_reduce = 1; /*min counts for word from train to stay in vocab*/

int * vocab_hash;

long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100;
long long train_words = 0, word_count_actual = 0, file_size = 0, classes = 0;
real alpha = 0.025, starting_alpha, sample = 0;
real *syn0, *syn1, *syn1neg, *expTable;
clock_t start;

// unigram table - hashing the unigram in vocab table
int hs = 1, negative = 0;
const int table_size = 1e8;
int * table;

// initialize unigram table
void InitUnigramTable() {
	int a, i; // a for general iteration
	long long train_words_pow = 0; // total power of words_cnt in vocab
	real d1, power = 0.75;
	// allocat space for unigram table
	table = (int *)malloc(table_size * sizeof(int));
	// find the total power of word count
	// to be used as the normalization factor
	// ITERATING vocab table, NOTE vocab_size will be decided later
	for (a = 0; a < vocab_size; a++) {
		train_words_pow += pow(vocab[a].cn, power);
	}
	// d1 - the power of count of the current ref element in vocab
	i = 0;
	d1 = pow(vocab[i].cn, power) / (real)train_words_pow; 
	// ITERATING unigram table, the table_size is prefixed
	for (a = 0; a < table_size; a++) {
		table[a] = i;
		// move to the next bin if index in vocab talbe exceeds the neighbord
		// specified by d1
		if (a / (real)table_size > d1) {
			i++;
			d1 += pow(vocab[i].cn, power) / (real)train_words_pow;
		}
		// put everthing else in the end of the unigram table
		if (i >= vocab_size) {
			i = vocab_size - 1;
		}
	}
}

void ReadWord(char * word, FILE * fin) {
	// Reads a single word from a file
	// assuming SPACE + TAB + EOL to be word boundaries
	// </s> will be the first word in each voc file??!!
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if (ch == 13) continue; // carriage return, new line
		if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
			if (a > 0) {
				if (ch == '\n') ungetc(ch, fin);
				break;
			}
			// end of the words stream
			if (ch == '\n') {
				strcpy(word, (char *)"</s>");
				return;
			} else continue;
		}
		word[a] = ch;
		a++;
		if (a >= MAX_STRING - 1) a--; // Truncate too long words
	}
	word[a] = 0;
}

int GetWordHash(char * word) {
	unsigned long long a, hash = 0;
	// 257 - the smallest prime greater than 255 (1 byte)
	for (a = 0; a < strlen(word); a++)
		hash = hash * 257 + word[a];
	hash = hash % vocab_hash_size;
	return hash;
}

int AddWordToVocab(char * word) {
	// Adds a word to the vocabulary
	unsigned int hash, length = strlen(word) + 1;
	if (length > MAX_STRING) length = MAX_STRING; // Truncation
	vocab[vocab_size].word = (char *) calloc(length, sizeof(char));
	strcpy(vocab[vocab_size].word, word);
	// cn are initialized to 0s because
	// it will be read later from the vocabulary file
	vocab[vocab_size].cn = 0;
	vocab_size++;
	// relocate memeory if needed
	if (vocab_size + 2 >= vocab_max_size) {
		vocab_max_size += 1000;
		vocab = (struct vocab_word *) realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
	}
	// hashing value for the current word
	hash = GetWordHash(word);
	// increase hash by 1 until it finds an empty slot in vocab_hash !!
	// Potetially, if the size of vocab_hash is smaller than the size of vocab
	// it could never find an empty slot
	// vocab_hash was intialized earlier in ReadVocab()
	while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
	vocab_hash[hash] = vocab_size - 1;
	return vocab_size - 1;
}

int VocabCompare(const void * a, const void * b) {
	// comparing words by their word counts
	return ((struct vocab_word * )b)->cn - ((struct vocab_word * )a)->cn;
}

void SortVocab() {
	// Sorts the vocabulary by frequency using word counts
	int a, size;
	unsigned int hash;
	// sort the vocabulary and keep </s> at the first position
	// in Decreasing order
	qsort(&vocab[1], vocab_size-1, sizeof(struct vocab_word), VocabCompare);
	// vocab_hash was earlier initialized in ReadVocab()
	// and it is REinitialized here to be -1 BECAUSE we are
	// sorting the words and invalidating their previous index
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	size = vocab_size; // because vocab_size changes along the loop
	train_words = 0;
	// OUTPUT OF THE LOOP: ALL INFREQUNET WORDS DELETED, vocab_size, and
	// train_words size are all correct
	for (a = 0; a < size; a++) {
		// words occuring less than min_count times will be discared from the vocab
		// BUT HERE </s> WILL STILL REMAIN - because the words are removed from
		// the end of vocab
		// AND </s> IS NOT HASHED IN vocab_hash -- its index is still -1
		if (vocab[a].cn < min_count) {
			vocab_size--;
			// NOT vocab[a].word but vocab[vocab_size].word ?? A BUG ??
			// NO actually it works here because the words are now
			// sorted in a decreasing order (wrt word counts), so after 
			// finding the first occurance of < min_count, all the
			// occurances should just be behind it.
			free(vocab[vocab_size].word);
		} else {
			// hash will be re-computed, as after the sorting it is not valid anymore
			// why recomputing of the word hash is needed???
			// as hashing of the word is based on the string itself and vocab_hash_size, and has
			// nothing to do with the index of the word in the vocab
			// SO THE ONLY REASON why "hash" is needed again (because it is not stored previously),
			// is that now the hash table is filled as MOST_FREQUENT_WORD_TAKES_PRIORITY (empty slot)
			// compared to previously FIRST_COMING_WORD_TAKES_PRIORITY in AddWordToVocab()
			hash = GetWordHash(vocab[a].word);
			while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
			vocab_hash[hash] = a;
			train_words += vocab[a].cn;
			// BUT AS A RESULT, the word index used in the vocab_hash are the same
			// index (after sorted by word counts) AS IF the infrequent words ARE 
			// STILL IN VOCAB ??? 
			// NO it works, because all the words discarded are in the rear
			// of the list, so the index in vocab_hash ARE CONSISTENT with 
			// the index in vocab, see comments above as well.
		}
	}
	// now it makes sense to make the vocab table shrink 
	// by just free the rear part of the table
	// MUST BE VOCAB_SIZE + 1 because </s> is there
	vocab = (struct vocab_word *)realloc(vocab, (vocab_size+1) * sizeof(struct vocab_word));
	// prepare memory for binary tree construction
	for (a = 0; a < vocab_size; a++) {
		vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
		vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
	}
}

void ReadVocab() {
	long long a, i = 0;
	char c;
	char word[MAX_STRING];
	FILE * fin = fopen(read_vocab_file, "rb");
	if (fin == NULL) {
		printf("Vocabulary file not found\n");
		exit(1);
	}
	// initiliaze the hash table -1 for all words
	for (a = 0; a < vocab_hash_size; a++)
		vocab_hash[a] = -1;
	vocab_size = 0;
	// read all the words, and their counts from the file
	// add them in the vocab, add their index to vocab_hash
	// i number of words in vocab
	while (1) {
		ReadWord(word, fin);
		if (feof(fin)) break;
		// suppose the words in vocab are already unique
		// add word structure to vocab,
		// put their index in vocab in vocab_hash
		a = AddWordToVocab(word); // index of the word in vocab
		// swallow c - the new line "\n"
		fscanf(fin, "%lld%c", &vocab[a].cn, &c);
		i++;
	}
	// sort the words by their freq. (word counts)
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	fseek(fin, 0, SEEK_END);
	file_size = ftell(fin);
	fclose(fin);
}

int SearchVocab(char * word) {
	unsigned int hash = GetWordHash(word);
	while (1) {
		// no found
		if (vocab_hash[hash] == -1) return -1; 
		// return hit index
		if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
		// keep searching when no hit and no miss yet 
		hash = (hash + 1) % vocab_hash_size;
	}
	return -1; // never reach here
}

void ReduceVocab() {
	// reduces the vocabulary by removing infrequent tokens.
	int a, b = 0;
	unsigned int hash;
	// The in-place removal code is FANTASTIC!!
	for (a = 0; a < vocab_size; a++) {
		if (vocab[a].cn > min_reduce) {
			vocab[b].cn = vocab[a].cn;
			vocab[b].word = vocab[a].word;
			b++;
		} else {
			free (vocab[a].word);
		}
	}
	vocab_size = b;
	// reset the hash table
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	for (a = 0; a < vocab_size; a++) {
		// Hash will be re-computed, as it is not actual
		hash = GetWordHash(vocab[a].word);
		while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
		vocab_hash[hash] = a;
	}
	fflush(stdout);
	// Incerease the threshold next time
	// so it wont be a for-ever loop twisted with LearnVocabFromTrainFile
	min_reduce++;
}

void LearnVocabFromTrainFile() {
	char word[MAX_STRING];
	FILE * fin;
	long long a, i;
	// initialize hash table as all -1s
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	vocab_size = 0;
	// always add </s> as the first one - otherwise SortVocab will be wrong
	// THis is consistent with ReadVocab()
	AddWordToVocab((char *)"</s>");
	while (1) {
		// WILL INSERT </S> FOR EACH NEW LINE
		ReadWord(word, fin);
		// ReadWord, but no fscanf(fin, "%lld%c", ...); as in ReadVocab file
		if (feof(fin)) break;
		train_words++;
		if ((debug_mode > 1) && (train_words % 100000 == 0)) {
			printf("%lldK%c", train_words / 1000, 13);
			fflush(stdout);
		}
		// find the index of word in vocab by searching in vocab_hash
		i = SearchVocab(word);
		// no found in vocab - add to vocab and vocab_hash, set word.cn = 1
		// found in vocab - update word.cn += 1
		if (i == -1) {
			a = AddWordToVocab(word);
			vocab[a].cn = 1;
		} else vocab[i].cn++;
		// vocab is too LARGE for the current vocab_hash_table
		if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
	}
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	file_size = ftell(fin);
	fclose(fin);
}

void SaveVocab() {
	long long i;
	FILE * fo = fopen(save_vocab_file, "wb");
	// "</s> 1\n" will be the first line of the written voc file
	for (i = 0; i < vocab_size; i++) {
		fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
	}
	fclose(fo);
}

void CreateBinaryTree() {
	//Create binary Huffman tree using the word counts
	// Frequent words will have short unique binary codes
	long long a, b, i;
	long long min1i, min2i; // two smallest nodes
	long long pos1, pos2; // current pivots
	long long point[MAX_CODE_LENGTH];
	char code[MAX_CODE_LENGTH];
	// calloc initializes the memory to zeros
	// SHOULD IT BE vocab_size * 2 - 1 - this is because
	// it seems that </s> is in part of construction, but vocab_size is 
	// actually len(vocab)-1, excluding </s>
	long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
	// count - word counts of all words
	for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;
	// extend count as twice large
	// SO ONLY vocab_size * 2 - 1 elements will BE NEEDED, EVEN FOR A 
	// COMPLETE TREE
	for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;
	// initialize the node positions
	pos1 = vocab_size - 1; 
	pos2 = vocab_size;
	// following algorithm constructs the Huffman tree by
	// adding one node at a time
	// the vocab should have been sorted IN DECREASING order
	// pos1 and pos2 will be the two current smallest node 
	// they could be the original elements, they could be composed parent nodes
	// the parents node will be constructed and placed along the array
	// from [vocab_size to vocab_size * 2 - 1].
	// THE GOOD THING IS: the elements on the left of pos1 are all SORTED, 
	// and the elements on the right of pos2 will also be SORTED.

	// THE LAST WORD </s> WILL ALSO BE INCLUDED IN THE TREE
	// ONLY NEED TO CONSTRUCT vocab_size - 1 times, that is max number 
	// of parent nodes for a complete binary tree
	for (a = 0; a < vocab_size - 1; a++) {
		// First, find two smallest nodes "min1, min2"
		// MIN1 goes first
		if (pos1 >= 0) {
			if (count[pos1] < count[pos2]) { // move left via pos1
				min1i = pos1;
				pos1--;
			} else { // move right via pos2
				min1i = pos2;
				pos2++;
			}
		} else { // no choice, can move right ONLy now
			min1i = pos2;
			pos2++;
		}
		// MIN2 goes next
		if (pos1 >= 0) {
			if (count[pos1] < count[pos2]) { // move left via pos1
				min2i = pos1;
				pos1--;
			} else { // move right via pos2
				min2i = pos2;
				pos2++;
			}
		} else {
			min2i = pos2;
			pos2++;
		}
		// parent's count is the sum of children's counts
		count[vocab_size + a] = count[min1i] + count[min2i];
		// commmon parents
		// level 2 parents will be from vocab_size to vocab_size * 2
		parent_node[min1i] = vocab_size + a;
		parent_node[min2i] = vocab_size + a;
		// binary code: min1i 0 min2i 1, for each leaf and internal node
		binary[min2i] = 1;
	}
	// now assign binary code to each vocabulary word
	// update each vocab word and its parent
	for (a = 0; a < vocab_size; a++) {
		b = a;
		i = 0;
		// upstreaming to parent of each leave (a) and its parent (b)
		// to update their code and point - they are temprory structure
		// whose max length is the tranverse length
		// * point is an array of indices in count, in an the order of 
		//   traversing the tree from each leaf to its parents up to the root
		// * code is an array 
		while (1) {
			code[i] = binary[b];
			point[i] = b;

			i++; // the depth of traverse from leaf to root
			b = parent_node[b];
			if (b == vocab_size * 2 - 2) break;
		}
		vocab[a].codelen = i; // depth
		// point - relative index of parent from vocab_size+1
		// in reverse order - path from root to the current word (leaf node)
		vocab[a].point[0] = vocab_size - 2; 
		for (b = 0; b < i; b++) {
			vocab[a].code[i - b - 1] = code[b];
			vocab[a].point[i - b] = point[b] - vocab_size; // parent node index
		}
	}
	free(count);
	free(binary);
	free(parent_code);
}

int ReadWordIndex(FILE * fin) {
	// Reads a word and returns its index in the vocabulary
	char word[MAX_STRING];
	ReadWord(word, fin);
	if (feof(fin)) return -1;
	return SearchVocab(word);
}

// IT SEEMS that hs and negative can be used TOGETHER
void InitNet() {
	// intialize the neural network structure
	long long a, b;
	// SOME CONVENTIONS : layer1_size will the the dimension of feature space
	// syn0 and syn1/syn1neg are of size vocab_size * layer1_size
	// allocate the memory to syn0, a stores return code
	// syn0 is actually of (real *)
	a = posix_memlign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
	if (syn0 == NULL) {
		printf("Memory allocation failed\n"); exit(1);
	}
	// Hierarchical Softmax 
	if (hs) {
		// allocate memory to syn1, a stores return code
		a = posix_memlign((void **)&syn1, 128, (long long)vocab_size * layer1_size * sizeof(real));
		if (syn1 == NULL) {printf("Memory allocation failed\n"); exit(1);}
		for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
			syn1[a * layer1_size + b] = 0;
	}
	// Negative Sampling
	if (negative > 0) {
		a = posix_memlign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
		if (syn1neg == NULL) {printf("Memory allocaiton failed\n"); exit(1);}
		for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
			syn1neg[a * layer1_size + b] = 0;
	}
	// Initialization of syn0 layer to [-0.5, 0.5] / layer1_size
	// 0 mean, 0.0015 std, though it is a uniform distribution
	for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
		syn0[a * layer1_size + b] = (rand() / (real)RAND_MAX - 0.5) / layer1_size;
	CreateBinaryTree();
}

// learning: hs (hierarchical softmax) v.s. negative sampling
// model: cbow v.s. skip gram
void *TrainModelThread(void *id) {
	long long a, b, d, word, last_word, sentence_length = 0, sentence_position = 0;
	long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
	long long l1, l2, c, target, label;
	// id is NOT a pointer to an address, it itself is an interger (long, long)
	// see the caller function for details
	unsigned long long next_random = (long long) id;
	real f, g; // function and gradient
	clock_t now;
	// hidden output, neu1 is a vector, input syn0 is an matrix (collection of vectors)
	real * neu1 = (real *)calloc(layer1_size, sizeof(real));
	// ?? error of 
	real * neu1e = (real *)calloc(layer1_size, sizeof(real));
	// embarassingly parallel model - chunk the data file
	// synchoronize on global structure of net 
	FILE * fi = fopen(train_file, "rb");
	fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
	// RELATED VARIABLES: 
	// word, last_word, word_count, last_word_count (word_count_actual local copy)
	// sentence_length, sentence_position
	// word_count_actual - global variable to share among different threads
	while (1) {
		// use word_count to control learning rate (decreasing and converging)
		// every time when another 10000 words have been counted
		if (word_count - last_word_count > 10000) {
			word_count_actual += word_count - last_word_count;
			last_word_count = word_count;
			if (debug_mode > 1) {
				now = clock();
				printf("%cAlpah: %f Progress: %.2f%% Words/thread/sec: %.2fk ", 13, alpha, 
					word_count_actual / (real)(train_words + 1) * 100,
					word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
				fflush(stdout);
			}
			alpha = strarting_alpha * (1 - word_count_actual / (real)(train_words + 1));
			if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
		}
		// if sen is empty, create the sentence by reading words from file
		// and add their vocab index to sen, initialize sentence_position = 0
		// Discarding some infrequent words based on subsampling  
		// ONLY read when sen is EMPTY AGAIN and REFILL IT
		if (sentence_length == 0) {
			while(1) {
				// read a word from file chunk and find the its index in vocab
				word = ReadWordIndex(fi);
				// end of word stream
				if (feof(fi)) break;
				// word not found in vocab
				if (word == -1) continue;
				word_count++;
				// the </s>, which marks the end?
				if (word == 0) break;
				// the subsampling randomly discards infrequent
				// words while keeping the ranking same
				if (sample > 0) {
					real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
					next_random = next_random * (unsigned long long)25214903917 + 11;
					if (ran < (next_random & 0xFFFF) / (real)65536) continue;
				}
				// add word (index) to sen
				sen[sentence_length] = word;
				sentence_length++;
				// sentence is full
				if (sentence_length >= MAX_SENTENCE_LENGTH) break;
			}
			sentence_position = 0;
		}
		// end of file or exceeds to the next chunk of data - stop
		if (feof(fi)) break;
		if (word_count > train_words / num_threads) break;
		// for word(index) in sentence
		word = sen[sentence_position];
		// no word at all - should NOT get into sen in the first place
		if (word == -1) continue;
		// initialize neu1 and its error
		for (c = 0; c < layer1_size; c++) neu1[c] = 0;
		for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
		// comments on random seed - 
		// http://ozark.hendrix.edu/~burch/logisim/docs/2.3.0/libs/mem/random.html
		next_random = next_random * (unsigned long long)25214903917 + 11;
		// b defines the bound (randomly) with a of the word window in sen.
		// a is window * 2 + 1 - b
		// WINDOW OFFSET - the new window size will be (window - b)
		b = next_random % window; 

		if (cbow) { // train the cbow architecture - HS or NS
			// IN -> HIDDEN
			// c in [sentence_position - (window-b), sentence_position + (window-b)]
			for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
				// bypass the word sen[c] that is beyond the sentence range
				c = sentence_position - window + a;
				if (c < 0) continue;
				if (c >= sentence_length) continue;
				// last_word: those in the sliding window around word
				last_word = sen[c];
				if (last_word == -1) continue;
				// for each hidden neu1[c], it is the sum of 
				// several input syn0[c + lastword_i]
				// the lastword_i s define a window
				// syn0 SIZE: vocab_size*layer1_size, neu1 SIZE: layer1_size
				// accumulate the feats of lastword in syn0 to neu1
				// last word be iterating from the sliding window [-(window-b), +(window+b)] 
				// around current word (sen[sentence_position] or word)
				for (c = 0; c < layer1_size; c++) 
					neu1[c] += syn0[c + last_word * layer1_size];
			}
			// HIERARCHICAL SOFTMAX
			// PRECONDITION: word = sen[sentence_position]
			if (hs) for (d = 0; d < vocab[word].codelen; d++) {
				f = 0; // OBJECTIVE function
				// the feature start in syn0 for parent node of vocab[word]
				l2 = vocab[word].point[d] * layer1_size;
				// propagate hidden -> output
				for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1[c + l2];
				if (f <= -MAX_EXP) continue;
				else if (f >= MAX_EXP) continue;
				else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
				// g is the gradient multiplied by the learning rate
				g = (1 - vocab[word].code[d] -f) * alpha;
				// propogate errors output -> hidden
				for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
				// learning weights hidden -> output
				for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * neu1[c];
			}
			// NEGATIVE SAMPLING
			if (negative > 0) for (d = 0; d < negative + 1; d++) {
				//TODO
			}
			// HIDDEN -> IN
			for (a = b; a < window * 2 + 1 -b; a++) if (a != window) {
				//TODO
			}
		} else { // train skip-gram
			//TODO
		}
		// next word in sen or SIMPLY refill from file
		sentence_position++;
		if (sentence_position >= sentence_length) {
			sentence_length = 0;
			continue;
		}
	}
	fclose(fi);
	free(neu1);
	free(neu1e);
	pthread_exit(NULL);
}

void TrainModel(){
	long a, b, c, d;
	FILE * fo;
	// threads objects
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
	printf("Starting training using file %s\n", train_file);

	starting_alpha = alpha;
	// build vocab either from vocab file or train file
	if (read_vocab_file[0] != 0) ReadVocab(); else LearnVocabFromTrainFile();
	// save it if required
	if (save_vocab_file[0] != 0) SaveVocab();
	if (output_file[0] == 0) return;

	InitNet();

	if (negative > 0) InitUnigramTable(); // negative sampling

	// create threads to do training and block-wait
	start = clock();
	// pass a instead of &a, as "a" is a local variable
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);

	// write output file
	fo = fopen(output_file, "wb");
	// save word vectors
	if (classes == 0) {
		fprintf(fo, "%lld %lld\n", vocab_size, layer1_size);
		for (a = 0; a < vocab_size; a++) {
			fprintf(fo, "%s ", vocab[a].word);
			if (binary) {
				for (b = 0; b < layer1_size; b++) 
					fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
			} else {
				for (b = 0; b < layer1_size; b++)
					fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
			}
			fprintf(fo, "\n");
		}
	} else { // save the word classes
		// run kmeans on word vectors to get word classes
		int clcn = classes, iter = 0, closeid;
		// sizes of each cluster
		int *centcn = (int *)malloc(classes * sizeof(int));
		// classes of each word in vocab
		int *cl = (int *)calloc(vocab_size, sizeof(int));
		real closev, x;
		// center vector
		real *cent = (real *)calloc(classes * layer1_size, sizeof(real));
		// initialize class labels of words in a wheel way
		for (a = 0; a < vocab_size; a++) cl[a] = a % clcn;
		// iterative training
		for (a = 0; a < iter; a++) {
			// reset centers to all zeros
			for (b = 0; b < clcn * layer1_size; b++) cent[b] = 0;
			// reset cluster size to 1 element
			for (b = 0; b < clcn; b++) centcn[b] = 1;
			// for each word (for each feature of it)
			// center_vec += word_vec
			// center_size += 1
			for (c = 0; c < vocab_size; c++) {
				for (d = 0; d < layer1_size; d++) {
					// cl[c] is the cluster index of word at c
					cent[layer1_size * cl[c] + d] += syn0[c * layer1_size + d];
					centcn[cl[c]]++;
				}
			}
			// for each cluster (for each feature of cluster center)
			// cent_vec /= cluster_size
			// cent_vec `~ normalized by l2 norm
			for (b = 0; b < clcn; b++) {
				closev = 0;
				for (c = 0; c < layer1_size; c++) {
					// taking average
					cent[layer1_size * b + c] /= centcn[b];
					closev += cent[layer1_size * b + c] * cent[layer1_size * b + c];
				}
				// closev = l2 norm of the center vec
				// normalize the center vec by its l2 norm
				// NORMALIZATION OF CENTER VECTORS FOR LATER DISTANCE COMPARISON
				closev = sqrt(closev);
				for (c = 0; c < layer1_size; c++) cent[layer1_size * b + c] /= closev;
			}
			// ASSIGN each word to the corresponding center
			// for each word, for each cluster, 
			// calculate the dist between the word vec and the cluster center 
			// (cluster vecs have all been normalized, so just use the inner product)
			// find the closest cluster center to the current word vector
			// closev (the closest dist so far), closeid (the closest cluster id so far)
			for (c = 0; c < vocab_size; c++) {
				closev = -10;
				closeid = 0;
				for (d = 0; d < clcn; d++) {
					x = 0;
					for (b = 0; b < layer1_size; b++)
						x += cent[layer1_size * d + b] * syn0[c * layer1_size + b];
					if (x > closev) {
						closev = x;
						closeid = d;
					}
				}
				cl[c] = closeid;
			}
		}
		// save the kmeans classes
		for (a = 0; a < vocab_size; a++)
			fprintf(fo, "%s %d\n", vocab[a].word, cl[a]);
		free(centcn);
		free(cent);
		free(cl);
	}
	fclose(fo);
}

// parse the command line arguments
// helper function - find the position of the argument
int ArgPos(char * str, int argc, char ** argv) {
	int a;
	for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
		if (a == argc - 1) {
			printf("Augument missing for %s\n", str);
			exit(1);
		}
		return a;
	}
	return -1;
}

// main entry
int main(int argc, char ** argv) {
	int i;
	// helper message
	if (argc == 1) {
    printf("WORD VECTOR estimation toolkit v 0.1b\n\n");
    printf("Options:\n");
    printf("Parameters for training:\n");
    printf("\t-train <file>\n");
    printf("\t\tUse text data from <file> to train the model\n");
    printf("\t-output <file>\n");
    printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
    printf("\t-size <int>\n");
    printf("\t\tSet size of word vectors; default is 100\n");
    printf("\t-window <int>\n");
    printf("\t\tSet max skip length between words; default is 5\n");
    printf("\t-sample <float>\n");
    printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency");
    printf(" in the training data will be randomly down-sampled; default is 0 (off), useful value is 1e-5\n");
    printf("\t-hs <int>\n");
    printf("\t\tUse Hierarchical Softmax; default is 1 (0 = not used)\n");
    printf("\t-negative <int>\n");
    printf("\t\tNumber of negative examples; default is 0, common values are 5 - 10 (0 = not used)\n");
    printf("\t-threads <int>\n");
    printf("\t\tUse <int> threads (default 1)\n");
    printf("\t-min-count <int>\n");
    printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    printf("\t-alpha <float>\n");
    printf("\t\tSet the starting learning rate; default is 0.025\n");
    printf("\t-classes <int>\n");
    printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
    printf("\t-debug <int>\n");
    printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
    printf("\t-binary <int>\n");
    printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
    printf("\t-save-vocab <file>\n");
    printf("\t\tThe vocabulary will be saved to <file>\n");
    printf("\t-read-vocab <file>\n");
    printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    printf("\t-cbow <int>\n");
    printf("\t\tUse the continuous back of words model; default is 0 (skip-gram model)\n");
    printf("\nExamples:\n");
    printf("./word2vec -train data.txt -output vec.txt -debug 2 -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1\n\n");
    return 0;
  }
  // initialize the file pathes to empty strings
  output_file[0] = 0;
  save_vocab_file[0] = 0;
  read_vocab_file[0] = 0;
  // parse the arguments 
  if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);
  // allocate memory for vocab, vocab_hash, and expTable table
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  // precomputing exponetial table 
  expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
  for (i = 0; i < EXP_TABLE_SIZE; i++) {
  	// Precompute the exp() table
  	expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP);
  	// Precompute f(x) = x / (x + 1)
  	expTable[i] = expTable[i] / (expTable[i] + 1);
  }
  TrainModel();
  return 0;
}