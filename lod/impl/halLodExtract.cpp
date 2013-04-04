/*
 * Copyright (C) 2013 by Glenn Hickey (hickey@soe.ucsc.edu)
 *
 * Released under the MIT license, see LICENSE.txt
 */

#include <cassert>
#include <deque>
#include "halLodExtract.h"
extern "C" {
#include "sonLibTree.h"
}
 

using namespace std;
using namespace hal;

LodExtract::LodExtract()
{

}

LodExtract::~LodExtract()
{
  
}

void LodExtract::createInterpolatedAlignment(AlignmentConstPtr inAlignment,
                                             AlignmentPtr outAlignment,
                                             hal_size_t step,
                                             const string& tree)
{
  _inAlignment = inAlignment;
  _outAlignment = outAlignment;
  
  string newTree = tree.empty() ? inAlignment->getNewickTree() : tree;
  createTree(newTree);
  cout << "tree = " << _outAlignment->getNewickTree() << endl;
  
  deque<string> bfQueue;
  bfQueue.push_front(_outAlignment->getRootName());
  while (!bfQueue.empty())
  {
    string genomeName = bfQueue.back();
    bfQueue.pop_back();
    vector<string> childNames = _outAlignment->getChildNames(genomeName);
    if (!childNames.empty())
    {
      convertInternalNode(genomeName, step);
      for (size_t childIdx = 0; childIdx < childNames.size(); childIdx++)
      {
        bfQueue.push_back(childNames[childIdx]);
      } 
    }
  }
}

void LodExtract::createTree(const string& tree)
{
  if (_outAlignment->getNumGenomes() != 0)
  {
    throw hal_exception("Output alignment not empty");
  }
  stTree* root = stTree_parseNewickString(const_cast<char*>(tree.c_str()));
  assert(root != NULL);
  deque<stTree*> bfQueue;
  bfQueue.push_front(root);
  while (!bfQueue.empty())
  {
    stTree* node = bfQueue.back();
    const char* label = stTree_getLabel(node);
    if (label == NULL)
    {
      throw hal_exception("Error parsing tree: unlabeled node");
    }
    const Genome* test = _inAlignment->openGenome(label);
    if (test == NULL)
    {
      throw hal_exception(string("Genome in tree: ") + string(label) +
                          "doesn't exist in source alignment");
    }
    else
    {
      _inAlignment->closeGenome(test);
    }    
    if (node == root)
    {
      _outAlignment->addRootGenome(label);
    }
    else
    {
      stTree* parent = stTree_getParent(node);
      assert(parent != NULL);
      const char* pLabel = stTree_getLabel(parent);
      assert(pLabel != NULL);
      double branchLength = stTree_getBranchLength(node);
      // clamp undefined branch lengths to 1. for now
      if (branchLength > 1e10)
      {
        branchLength = 1.;
      }
      _outAlignment->addLeafGenome(label, pLabel, branchLength);
    }

    int32_t numChildren = stTree_getChildNumber(node);
    for (int32_t childIdx = 0; childIdx < numChildren; ++childIdx)
    {
      bfQueue.push_front(stTree_getChild(node, childIdx));
    }

    bfQueue.pop_back();
  }
  stTree_destruct(root);
}

void LodExtract::convertInternalNode(const string& genomeName, 
                                      hal_size_t step)
{
  const Genome* parent = _inAlignment->openGenome(genomeName);
  assert(parent != NULL);
  vector<string> childNames = _outAlignment->getChildNames(genomeName);
  vector<const Genome*> children;
  for (hal_size_t i = 0; i < childNames.size(); ++i)
  {
    children.push_back(_inAlignment->openGenome(childNames[i]));
  }
  _graph.build(_inAlignment, parent, children, step);

  map<const Sequence*, hal_size_t> segmentCounts;
  countSegmentsInGraph(segmentCounts);

  writeDimensions(segmentCounts, parent->getName(), childNames);
  writeSegments(parent, children);
  writeHomologies(parent, children);
  writeParseInfo(_outAlignment->openGenome(parent->getName()));

  // if we're gonna print anything out, do it before this:
  // (not necesssary but by closing genomes we erase their hdf5 caches
  // which can make a difference on huge trees
  _graph.erase();
  _outAlignment->closeGenome(_outAlignment->openGenome(parent->getName()));
  _inAlignment->closeGenome(parent);
  for (hal_size_t i = 0; i < children.size(); ++i)
  {
    _outAlignment->closeGenome(
      _outAlignment->openGenome(children[i]->getName()));
    _inAlignment->closeGenome(children[i]);    
  }
}

void LodExtract::countSegmentsInGraph(
  map<const Sequence*, hal_size_t>& segmentCounts)
{
  const LodBlock* block;
  const LodSegment* segment;
  pair<map<const Sequence*, hal_size_t>::iterator, bool> res;

  for (hal_size_t blockIdx = 0; blockIdx < _graph.getNumBlocks(); ++blockIdx)
  {
    block = _graph.getBlock(blockIdx);
    for (hal_size_t segIdx = 0; segIdx < block->getNumSegments(); ++segIdx)
    {
      segment = block->getSegment(segIdx);
      res = segmentCounts.insert(pair<const Sequence*, hal_size_t>(
                                   segment->getSequence(), 0));
      hal_size_t& count = res.first->second;
      ++count;
    }
  }
}

void LodExtract::writeDimensions(
  const map<const Sequence*, hal_size_t>& segmentCounts, 
  const string& parentName,
  const vector<string>& childNames)
{
  // initialize a dimensions list for each (input) genome
  map<const Genome*, vector<Sequence::Info> > dimMap;
  map<const Genome*, vector<Sequence::Info> >::iterator dimMapIt;
  vector<string> newGenomeNames = childNames;
  newGenomeNames.push_back(parentName);
  for (size_t i = 0; i < newGenomeNames.size(); ++i)
  {
    const Genome* inGenome = _inAlignment->openGenome(newGenomeNames[i]);
    pair<const Genome*, vector<Sequence::Info> > newEntry;
    newEntry.first = inGenome;
    dimMap.insert(newEntry);
  }
  
  // scan through the segment counts, adding the dimensions of each
  // sequence to the map entry for the appropriate genome. 
  map<const Sequence*, hal_size_t>::const_iterator segMapIt;
  for (segMapIt = segmentCounts.begin(); segMapIt != segmentCounts.end(); 
       ++segMapIt)
  {
    const Sequence* inSequence = segMapIt->first;
    const Genome* inGenome = inSequence->getGenome();
    dimMapIt = dimMap.find(inGenome);
    assert(dimMapIt != dimMap.end());
    vector<Sequence::Info>& segDims = dimMapIt->second;
    hal_size_t nTop = inGenome->getName() == parentName ? 0 : segMapIt->second;
    hal_size_t nBot = inGenome->getName() != parentName ? 0 : segMapIt->second;
    segDims.push_back(Sequence::Info(inSequence->getName(),
                                     inSequence->getSequenceLength(),
                                     nTop,
                                     nBot));
  }
  
  // now that we have the dimensions for each genome, update them in
  // the output alignment
  for (dimMapIt = dimMap.begin(); dimMapIt != dimMap.end(); ++dimMapIt)
  {
    Genome* newGenome = _outAlignment->openGenome(dimMapIt->first->getName());
    assert(newGenome != NULL);
    vector<Sequence::Info>& segDims = dimMapIt->second;

    // ROOT 
    if (newGenome->getName() == _outAlignment->getRootName())
    {
      assert(newGenome->getName() == parentName);
      newGenome->setDimensions(segDims);
    }
    // LEAF
    else if (newGenome->getName() != parentName)
    {
      newGenome->setDimensions(segDims);
    }
    // INTERNAL NODE
    else
    {
      vector<Sequence::UpdateInfo> updateInfo;
      for (size_t i = 0; i < segDims.size(); ++i)
      {
        updateInfo.push_back(
          Sequence::UpdateInfo(segDims[i]._name,
                               segDims[i]._numBottomSegments));
      }
      newGenome->updateBottomDimensions(updateInfo);
    }
  }
}

void LodExtract::writeSegments(const Genome* inParent,
                               const vector<const Genome*>& inChildren)
{
  vector<const Genome*> inGenomes = inChildren;
  inGenomes.push_back(inParent);
  const Genome* outParent = _outAlignment->openGenome(inParent->getName());
  assert(outParent != NULL && outParent->getNumBottomSegments() > 0);
  BottomSegmentIteratorPtr bottom;
  TopSegmentIteratorPtr top;
  SegmentIteratorPtr outSegment;

  // FOR EVERY GENOME
  for (hal_size_t i = 0; i < inGenomes.size(); ++i)
  {
    const Genome* inGenome = inGenomes[i];
    Genome* outGenome = _outAlignment->openGenome(inGenome->getName());

    if (outGenome != outParent)
    {
      top = outGenome->getTopSegmentIterator();
      outSegment = top;
    }
    else
    {
      bottom = outGenome->getBottomSegmentIterator();
      outSegment = bottom;
    }

    SequenceIteratorPtr outSeqIt = outGenome->getSequenceIterator();
    SequenceIteratorConstPtr outSeqEnd = outGenome->getSequenceEndIterator();
    
    // FOR EVERY SEQUENCE IN GENOME
    for (; outSeqIt != outSeqEnd; outSeqIt->toNext())
    {
      const Sequence* outSequence = outSeqIt->getSequence();
      const Sequence* inSequence = 
         inGenome->getSequence(outSequence->getName());
      const LodGraph::SegmentSet* segSet = _graph.getSegmentSet(inSequence);
      assert(segSet != NULL);
      LodGraph::SegmentSet::const_iterator segIt = segSet->begin();
      //skip left telomere
      ++segIt;
      // use to skip right telomere:
      LodGraph::SegmentSet::const_iterator segLast = segSet->end();
      --segLast;
      
      // FOR EVERY SEGMENT IN SEQUENCE
      for (; segIt != segLast; ++segIt)
      {
        // write the HAL array index back to the segment to make
        // future passes quicker. 
        (*segIt)->setArrayIndex(outSegment->getArrayIndex());
        outSegment->setCoordinates((*segIt)->getLeftPos(), 
                                   (*segIt)->getLength());
        outSegment->toRight();
      }
    }
  } 
}

void LodExtract::writeHomologies(const Genome* inParent,
                                 const vector<const Genome*>& inChildren)
{
  vector<const Genome*> inGenomes = inChildren;
  inGenomes.push_back(inParent);
  Genome* outParent = _outAlignment->openGenome(inParent->getName());
  assert(outParent != NULL && outParent->getNumBottomSegments() > 0);
  assert(inChildren.size() > 0);
  Genome* outChild = _outAlignment->openGenome(inChildren[0]->getName());
  BottomSegmentIteratorPtr bottom = outParent->getBottomSegmentIterator();
  TopSegmentIteratorPtr top = outChild->getTopSegmentIterator();

  // FOR EVERY BLOCK
  for (hal_size_t blockIdx = 0; blockIdx < _graph.getNumBlocks(); ++blockIdx)
  {
    SegmentMap segMap;
    const LodBlock* block = _graph.getBlock(blockIdx);

    for (hal_size_t segIdx = 0; segIdx < block->getNumSegments(); ++segIdx)
    {
      const LodSegment* segment = block->getSegment(segIdx);
      const Genome* genome = segment->getSequence()->getGenome();

      // ADD TO MAP
      pair<SegmentMap::iterator, bool> res = segMap.insert(
        pair<const Genome*, SegmentSet*>(genome, NULL));
      if (res.second == true)
      {
        assert(res.first->second == NULL);
        res.first->second = new SegmentSet();
      }
      res.first->second->insert(segment);    
    }      
    updateBlockEdges(inParent, segMap, block, bottom, top);
  }
}

void LodExtract::updateBlockEdges(const Genome* inParentGenome,
                                  SegmentMap& segMap,
                                  const LodBlock* block,
                                  BottomSegmentIteratorPtr bottom,
                                  TopSegmentIteratorPtr top)
{
  Genome* outParentGenome = bottom->getGenome();
  const LodSegment* rootSeg = NULL;
  SegmentSet* segSet;
  SegmentSet::iterator setIt;

  // Zap all segments in parent genome
  SegmentMap::iterator mapIt = segMap.find(inParentGenome);
  if (mapIt != segMap.end())
  {
    segSet = mapIt->second;
    assert(segSet != NULL);
    setIt = segSet->begin();
    for (; setIt != segSet->end(); ++setIt)
    {
      bottom->setArrayIndex(outParentGenome, (*setIt)->getArrayIndex());
      for (hal_size_t i = 0; i < bottom->getNumChildren(); ++i)
      {
        bottom->setChildIndex(i, NULL_INDEX);
        bottom->setTopParseIndex(NULL_INDEX);
      }
    }

    // Choose first segment as parent to all segments in the child genome
    setIt = segSet->begin();
    rootSeg = *(setIt);
    bottom->setArrayIndex(outParentGenome, (*setIt)->getArrayIndex());
  }

  // Do the child genomes
  SegmentSet::iterator nextIt;
  for (mapIt = segMap.begin(); mapIt != segMap.end(); ++mapIt)
  {
    if (mapIt->first != inParentGenome)
    {
      Genome* outChildGenome = 
         _outAlignment->openGenome(mapIt->first->getName());
      hal_index_t childIndex = outParentGenome->getChildIndex(outChildGenome);
      assert(childIndex >= 0);
      segSet = mapIt->second;
      assert(segSet != NULL);
      for (setIt = segSet->begin(); setIt != segSet->end(); ++setIt)
      {
        top->setArrayIndex(outChildGenome, (*setIt)->getArrayIndex());
        top->setBottomParseIndex(NULL_INDEX);

        // Connect to parent
        if (rootSeg != NULL)
        {
          top->setParentIndex(bottom->getArrayIndex());
          bool reversed = (*setIt)->getFlipped() == rootSeg->getFlipped();
          top->setParentReversed(reversed);
          if (setIt == segSet->begin())
          {
            bottom->setChildIndex(childIndex, top->getArrayIndex());         
            bottom->setChildReversed(childIndex, reversed);      
          }
        }
        else
        {
          top->setParentIndex(NULL_INDEX);
        }

        // Connect to next paralogy
        SegmentSet::iterator setNext = setIt;
        ++setNext;
        if (setNext == segSet->end())
        {
          setNext = segSet->begin();
        }
        if (setNext == setIt)
        {
          top->setNextParalogyIndex(NULL_INDEX);
        }
        else
        {
          top->setNextParalogyIndex((*setNext)->getArrayIndex());
        }
      }
    }
  }
}

void LodExtract::writeParseInfo(Genome* genome)
{
  if (genome->getParent() == NULL || genome->getNumChildren() == 0)
  {
    return;
  }

 // copied from CactusHalConverter::updateRootParseInfo() in
  // cactus2hal/src/cactusHalConverter.cpp 
  BottomSegmentIteratorPtr bottomIterator = 
     genome->getBottomSegmentIterator();
  TopSegmentIteratorPtr topIterator = genome->getTopSegmentIterator();
  BottomSegmentIteratorConstPtr bend = genome->getBottomSegmentEndIterator();
  TopSegmentIteratorConstPtr tend = genome->getTopSegmentEndIterator();

  while (bottomIterator != bend && topIterator != tend)
  {
    bool bright = false;
    bool tright = false;
    BottomSegment* bseg = bottomIterator->getBottomSegment();
    TopSegment* tseg = topIterator->getTopSegment();
    hal_index_t bstart = bseg->getStartPosition();
    hal_index_t bend = bstart + (hal_index_t)bseg->getLength();
    hal_index_t tstart = tseg->getStartPosition();
    hal_index_t tend = tstart + (hal_index_t)tseg->getLength();
    
    if (bstart >= tstart && bstart < tend)
    {
      bseg->setTopParseIndex(tseg->getArrayIndex());
    }
    if (bend <= tend || bstart == bend)
    {
      bright = true;
    }
        
    if (tstart >= bstart && tstart < bend)
    {
      tseg->setBottomParseIndex(bseg->getArrayIndex());
    }
    if (tend <= bend || tstart == tend)
    {
      tright = true;
    }

    assert(bright || tright);
    if (bright == true)
    {
      bottomIterator->toRight();
    }
    if (tright == true)
    {
      topIterator->toRight();
    }
  }
}
