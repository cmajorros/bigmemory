/*
 *  bigmemory: an R package for managing massive matrices using C,
 *  with support for shared memory.
 *
 *  Copyright (C) 2008 John W. Emerson and Michael J. Kane
 *
 *  This file is part of bigmemory.
 *
 *  bigmemory is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

#include <string>
#include <sstream>
#include <fstream>
#include <fcntl.h> // to remove files

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/uuid.hpp>
#include "MSCexceptions.h"

#include "BigMatrix.h"

using namespace std;
using namespace boost;
using namespace boost::interprocess;

template<typename T>
std::string ttos(T i)
{
  stringstream s;
  s << i;
  return s.str();
}

template<typename T>
void* CreateLocalSepColMatrix(long nrow, long ncol)
{
  T** pMat = new T*[ncol];
  int i;
  for (i=0; i < ncol; ++i) 
  {
    pMat[i] = new T[nrow];
  }
  return reinterpret_cast<void*>(pMat);
}

template<typename T>
void* CreateSepColMatrix(long nrow, long ncol)
{
  return reinterpret_cast<void*>(new T[nrow*ncol]);
}

bool LocalBigMatrix::create( const long numRow, const long numCol, 
  const int matrixType, const bool sepCols)
{
  _nrow = numRow;
  _ncol = numCol;
  _matType = matrixType;
  _sepCols = sepCols;
  try
  {
    if (_sepCols)
    {
      switch(_matType)
      {
        case 1:
          _matrix = CreateLocalSepColMatrix<char>(_nrow, _ncol);
          break;
        case 2:
          _matrix = CreateLocalSepColMatrix<short>(_nrow, _ncol);
          break;
        case 4:
          _matrix = CreateLocalSepColMatrix<int>(_nrow, _ncol);
          break;
        case 8:
          _matrix = CreateLocalSepColMatrix<double>(_nrow, _ncol);
      }
    }
    else
    {
      switch(_matType)
      {
        case 1:
          _matrix = reinterpret_cast<void*>(new char[_nrow*_ncol]);
          break;
        case 2:
          _matrix = reinterpret_cast<void*>(new short[_nrow*_ncol]);
          break;
        case 4:
          _matrix = reinterpret_cast<void*>(new int[_nrow*_ncol]);
          break;
        case 8:
          _matrix = reinterpret_cast<void*>(new double[_nrow*_ncol]);
      }
    }
  }
  catch (std::bad_alloc &ex)
  {
    return false;
  }
  return true;
}

template<typename T>
void DestroyLocalSepColMatrix( T** matrix, const long ncol)
{
  long i;
  for (i=0; i < ncol; ++i)
  {
    delete [] matrix[i];
  }
  delete matrix;
}

template<typename T>
void DestroyLocalMatrix( T* matrix )
{
  delete [] matrix;
}

void LocalBigMatrix::destroy()
{
  if (_matrix && _ncol && _nrow)
  {
    if (_sepCols)
    {
      switch(_matType)
      {
        case 1:
          DestroyLocalSepColMatrix(reinterpret_cast<char**>(_matrix), _ncol);
          break;
        case 2:
          DestroyLocalSepColMatrix(reinterpret_cast<short**>(_matrix), _ncol);
          break;
        case 4:
          DestroyLocalSepColMatrix(reinterpret_cast<int**>(_matrix), _ncol);
          break;
        case 8:
          DestroyLocalSepColMatrix(reinterpret_cast<double**>(_matrix), _ncol);
          break;
      }
    }
    else
    {
      switch(_matType)
      {
        case 1:
          DestroyLocalMatrix(reinterpret_cast<char*>(_matrix));
          break;
        case 2:
        DestroyLocalMatrix(reinterpret_cast<short*>(_matrix));
        break;
        case 4:
          DestroyLocalMatrix(reinterpret_cast<int*>(_matrix));
          break;
        case 8:
          DestroyLocalMatrix(reinterpret_cast<double*>(_matrix));
          break;
      }
    }
    _matrix=NULL;
    _ncol=0;
    _nrow=0;
  }
}

bool SharedBigMatrix::create_uuid()
{
  // See http://www.boost.org/doc/libs/1_36_0/libs/random/random_demo.cpp
  // for documentation about seed problems with random number based uuid.
  named_mutex mutex(open_or_create, "SharedBigMatrix_create_uuid");
  mutex.lock();
  _uuid = uuid::create().to_string();
  mutex.unlock();
  named_mutex::remove("SharedBigMatrix_create_uuid");
  return true;
}

bool SharedBigMatrix::read_lock( Columns &cols )
{
  _mutexLock.read_write_lock();
  unsigned long i;
  for (i=0; i < cols.size(); ++i)
  {
    _mutexPtrs[ cols[i] ]->read_lock();
  }
  _mutexLock.unlock();
  return true;
}

bool SharedBigMatrix::read_write_lock( Columns &cols )
{
  _mutexLock.read_write_lock();
  unsigned long i;
  for (i=0; i < cols.size(); ++i)
  {
    _mutexPtrs[ cols[i] ]->read_write_lock();
  }
  _mutexLock.unlock();
  return true;
}

bool SharedBigMatrix::unlock( Columns &cols )
{
//  _mutexLock.read_write_lock();
  unsigned long i;
  for (i=0; i < cols.size(); ++i)
  {
    _mutexPtrs[ cols[i] ]->unlock();
  }
//  _mutexLock.unlock();
  return true;
}

template<typename T>
void* CreateSharedSepMatrix( const std::string &sharedName, 
  MappedRegionPtrs &dataRegionPtrs, const long nrow, 
  const long ncol )
{
  T** pMat = new T*[ncol];
  long i;
  dataRegionPtrs.resize(ncol);
  for (i=0; i < ncol; ++i)
  {
    try
    {
//      shared_memory_object::remove( (sharedName+"_column_"+ttos(i)).c_str() );
      shared_memory_object shm(create_only, 
        (sharedName + "_column_" + ttos(i)).c_str(),
        read_write);
      shm.truncate( nrow*sizeof(T) );
      dataRegionPtrs[i] = MappedRegionPtr( new MappedRegion(shm, read_write) );
      pMat[i] = reinterpret_cast<T*>( dataRegionPtrs[i]->get_address());
    }
    catch (interprocess_exception &ex)
    {
      long j;
      for (j=0; j < i; ++j)
      {
        shared_memory_object::remove( (sharedName+"_column_"+ttos(j)).c_str());
      }
      delete pMat;
      return false;
    }
  }
  return reinterpret_cast<void*>(pMat);
}

bool CreateMutexes( MutexPtrs &mutexPtrs, const std::string &sharedName,
  const unsigned long ncol )
{
  unsigned long i;
  mutexPtrs.resize(ncol);
  for (i=0; i < ncol; ++i)
  {
    mutexPtrs[i] = MutexPtr( new BigMemoryMutex );
    mutexPtrs[i]->init( sharedName+"_column_"+ttos(i)+"mutex" );
  }
  return true;
}

template<typename T>
void* CreateSharedMatrix( const std::string &sharedName, 
  MappedRegionPtrs &dataRegionPtrs, const long nrow, const long ncol )
{
  try
  {
//    shared_memory_object::remove( (sharedName.c_str()) );
    shared_memory_object shm(create_only, sharedName.c_str(), read_write);
    shm.truncate( nrow*ncol*sizeof(T) );
    dataRegionPtrs.push_back(
      MappedRegionPtr(new MappedRegion(shm, read_write)));
  }
  catch (interprocess_exception &ex)
  {
    shared_memory_object::remove( (sharedName.c_str() ) );
    return NULL;
  }
  return dataRegionPtrs[0]->get_address();
}

bool SharedMemoryBigMatrix::create( const long numRow, 
  const long numCol, const int matrixType, 
  const bool sepCols )
{
  create_uuid();
  named_mutex mutex( open_or_create, (_uuid+"_counter_mutex").c_str() );
  mutex.lock();
  _nrow = numRow;
  _ncol = numCol;
  _matType = matrixType;
  _sepCols = sepCols;
  _sharedName=_uuid;
  _sharedCounter.init(_sharedName+"_counter");
  if (_sepCols)
  {
    switch(_matType)
    {
      case 1:
        _matrix = CreateSharedSepMatrix<char>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 2:
        _matrix = CreateSharedSepMatrix<short>(_sharedName, _dataRegionPtrs, 
          _nrow, _ncol);
        break;
      case 4:
        _matrix = CreateSharedSepMatrix<int>(_sharedName, _dataRegionPtrs, 
          _nrow, _ncol);
        break;
      case 8:
        _matrix = CreateSharedSepMatrix<double>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
    }
  }
  else
  {
    switch(_matType)
    {
      case 1:
        _matrix = CreateSharedMatrix<char>(_sharedName, _dataRegionPtrs, 
          _nrow, _ncol);
        break;
      case 2:
        _matrix = CreateSharedMatrix<short>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 4:
        _matrix = CreateSharedMatrix<int>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 8:
        _matrix = CreateSharedMatrix<double>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
    }
  }
  if (_matrix == NULL)
  {
    _sharedCounter.reset();
    mutex.unlock();
    named_mutex::remove((_sharedName+"_counter_mutex").c_str());
    return false;
  }
  CreateMutexes(_mutexPtrs, _sharedName, _ncol);
  _mutexLock.init( _sharedName+"_mutex_lock" );
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

template<typename T>
void* ConnectSharedSepMatrix( const std::string &uuid, 
  MappedRegionPtrs &dataRegionPtrs, const unsigned long nrow, 
  const unsigned long ncol )
{
  T** pMat = new T*[ncol];
  unsigned long i;
  for (i=0; i < ncol; ++i)
  {
    shared_memory_object shm(open_only,
      (uuid + "_column_" + ttos(i)).c_str(),
      read_write);
    dataRegionPtrs.push_back(
      MappedRegionPtr(new MappedRegion(shm, read_write)));
//    mapped_region region(shm, read_write);
    pMat[i] = reinterpret_cast<T*>(dataRegionPtrs[i]->get_address());
  }
  return reinterpret_cast<void*>(pMat);
}

template<typename T>
void* ConnectSharedMatrix( const std::string &sharedName, 
  MappedRegionPtrs &dataRegionPtrs, const unsigned long nrow, 
  const unsigned long ncol)
{
  shared_memory_object shm(open_only, sharedName.c_str(), read_write);
  dataRegionPtrs.push_back(MappedRegionPtr(new MappedRegion(shm, read_write)));
//  mapped_region region(shm, read_write);
  return reinterpret_cast<void*>(dataRegionPtrs[0]->get_address());
}

bool SharedMemoryBigMatrix::connect( const std::string &uuid, 
  const long numRow, const long numCol, const int matrixType, 
  const bool sepCols )
{
  named_mutex mutex( open_or_create, (uuid+"_counter_mutex").c_str() );
  mutex.lock();
  _uuid=uuid;
  _sharedName = _uuid;
  _nrow = numRow;
  _ncol = numCol;
  _matType = matrixType;
  _sepCols = sepCols;
  _sharedCounter.init(_sharedName+"_counter");
  if (_sepCols)
  {
    switch(_matType)
    {
      case 1:
        _matrix = ConnectSharedSepMatrix<char>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 2:
        _matrix = ConnectSharedSepMatrix<short>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 4:
        _matrix = ConnectSharedSepMatrix<int>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 8:
        _matrix = ConnectSharedSepMatrix<double>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
    }
  }
  else
  {
    switch(_matType)
    {
      case 1:
        _matrix = ConnectSharedMatrix<char>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 2:
        _matrix = ConnectSharedMatrix<short>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 4:
        _matrix = ConnectSharedMatrix<int>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
        break;
      case 8:
        _matrix = ConnectSharedMatrix<double>(_sharedName, _dataRegionPtrs,
          _nrow, _ncol);
    }
  }
  CreateMutexes(_mutexPtrs, _sharedName, _ncol);
  _mutexLock.init( _sharedName+"_mutex_lock" );
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

void DestroySharedSepMatrix( const std::string &uuid, const unsigned long ncol )
{
  unsigned long i;
  for (i=0; i < ncol; ++i)
  {
    shared_memory_object::remove((uuid+ "_column_" + ttos(i)).c_str());
  }
}
bool SharedMemoryBigMatrix::destroy()
{
  named_mutex mutex( open_or_create, (_sharedName+"_counter_mutex").c_str() );
  mutex.lock();
  _dataRegionPtrs.resize(0);
  if (_sepCols)
  {
    if (_sharedCounter.get() == 1)
    {
      DestroySharedSepMatrix(_uuid, _ncol);
    }
    if (_matrix)
    {
      switch (_matType)
      {
        case 1:
          delete [] reinterpret_cast<char**>(_matrix);
          break;
        case 2:
          delete [] reinterpret_cast<short**>(_matrix);
          break;
        case 4:
          delete [] reinterpret_cast<int**>(_matrix);
          break;
        case 8:
          delete [] reinterpret_cast<double**>(_matrix);
          break;
      }
    }
  }
  else
  {
    if ( _sharedCounter.get() == 1 )
    {
      shared_memory_object::remove(_uuid.c_str());
    }
  }
  if (_sharedCounter.get() == 1)
  {
    long i;
    for (i=0; i < static_cast<long>(_mutexPtrs.size()); ++i)
    {
      _mutexPtrs[i]->destroy();
    }
    _mutexLock.destroy();
  }
  _sharedCounter.reset();
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

template<typename T>
void* ConnectFileBackedSepMatrix( const std::string &sharedName,
  const std::string &filePath, MappedRegionPtrs &dataRegionPtrs, 
  const long nrow, const long ncol)
{
  T** pMat = new T*[ncol];
  long i;
  dataRegionPtrs.resize(ncol);
  for (i=0; i < ncol; ++i)
  {
    std::string columnName = filePath + sharedName + "_column_" + ttos(i);
    // Map the file to this process.
    file_mapping mFile(columnName.c_str(), read_write);
    dataRegionPtrs[i] = MappedRegionPtr(new MappedRegion(mFile, read_write));
    pMat[i] = reinterpret_cast<T*>(dataRegionPtrs[i]->get_address());
  }
  return reinterpret_cast<void*>(pMat);
}

template<typename T>
void* CreateFileBackedSepMatrix( const std::string &fileName, 
  const std::string &filePath, MappedRegionPtrs &dataRegionPtrs, 
  const long nrow, const long ncol )
{
  long i;
  for (i=0; i < ncol; ++i)
  {
    std::string columnName = filePath + fileName + "_column_" + ttos(i);
    // Create the files.
    std::filebuf fbuf;
    if (!fbuf.open( columnName.c_str(), std::ios_base::in | std::ios_base::out |
      std::ios_base::trunc | std::ios_base::binary ))
		{
			return NULL;
		}
    fbuf.pubseekoff( nrow*sizeof(T), std::ios_base::beg);
    // I'm not sure if I need this next line
    fbuf.sputc(0);
    fbuf.close();
  }
  return ConnectFileBackedSepMatrix<T>(fileName, filePath, dataRegionPtrs, 
    nrow, ncol);
}

template<typename T>
void* ConnectFileBackedMatrix( const std::string &fileName, 
  const std::string &filePath, MappedRegionPtrs &dataRegionPtrs, 
  const long nrow, const long ncol )
{
  file_mapping mFile((filePath+fileName).c_str(), read_write);
  dataRegionPtrs.push_back(
    MappedRegionPtr(new MappedRegion(mFile, read_write)));
  return reinterpret_cast<void*>(dataRegionPtrs[0]->get_address());
}

template<typename T>
void* CreateFileBackedMatrix( const std::string &fileName, 
  const std::string &filePath, MappedRegionPtrs &dataRegionPtrs, 
  const long nrow, const long ncol )
{
  // Create the file.
  std::filebuf fbuf;
  if (!fbuf.open( (filePath+fileName).c_str(), 
      std::ios_base::in | std::ios_base::out |
      std::ios_base::trunc | std::ios_base::binary ))
	{
		return NULL;
	}
  fbuf.pubseekoff( nrow*ncol*sizeof(T), std::ios_base::beg);
  // I'm not sure if I need this next line
  fbuf.sputc(0);
  fbuf.close();
  return ConnectFileBackedMatrix<T>(fileName, filePath,
    dataRegionPtrs, nrow, ncol);
}

bool FileBackedBigMatrix::create( const std::string &fileName, 
  const std::string &filePath, const long numRow, const long numCol, 
  const int matrixType, const bool sepCols, bool preserve )
{
  create_uuid();
  named_mutex mutex(open_or_create, (fileName+uuid()+"_counter_mutex").c_str());
  mutex.lock();
  _fileName = fileName;
  _sharedName=fileName+uuid();
  _sharedCounter.init(_sharedName+"_counter");
  _nrow = numRow;
  _ncol = numCol;
  _matType = matrixType;
  _sepCols = sepCols;
  _preserve = preserve;
  if (_sepCols)
  {
    switch(_matType)
    {
      case 1:
        _matrix = CreateFileBackedSepMatrix<char>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 2:
        _matrix = CreateFileBackedSepMatrix<short>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 4:
        _matrix = CreateFileBackedSepMatrix<int>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 8:
        _matrix = CreateFileBackedSepMatrix<double>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
    }
  }
  else
  {
    switch(_matType)
    {
      case 1:
        _matrix = CreateFileBackedMatrix<char>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 2:
        _matrix = CreateFileBackedMatrix<short>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 4:
        _matrix = CreateFileBackedMatrix<int>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 8:
        _matrix = CreateFileBackedMatrix<double>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
    }
  }
	if (!_matrix)
	{
		return false;
	}
  CreateMutexes(_mutexPtrs, _sharedName, _ncol);
  _mutexLock.init( _sharedName+"_mutex_lock" );
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

bool FileBackedBigMatrix::connect( const std::string &sharedName, 
  const std::string &fileName, const std::string &filePath, const long numRow, 
  const long numCol, const int matrixType, const bool sepCols, 
  const bool preserve )
{
  named_mutex mutex( open_or_create, (sharedName+"_counter_mutex").c_str() );
  mutex.lock();
  _sharedName=sharedName;
  _fileName=fileName;
  _nrow = numRow;
  _ncol = numCol;
  _matType = matrixType;
  _sepCols = sepCols;
  _preserve = preserve;
  _sharedCounter.init(_sharedName+"_counter");
  if (_sepCols)
  {
    switch(_matType)
    {
      case 1:
        _matrix = ConnectFileBackedSepMatrix<char>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 2:
        _matrix = ConnectFileBackedSepMatrix<short>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 4:
        _matrix = ConnectFileBackedSepMatrix<int>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 8:
        _matrix = ConnectFileBackedSepMatrix<double>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
    }
  }
  else
  {
    switch(_matType)
    {
      case 1:
        _matrix = ConnectFileBackedMatrix<char>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 2:
        _matrix = ConnectFileBackedMatrix<short>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 4:
        _matrix = ConnectFileBackedMatrix<int>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
        break;
      case 8:
        _matrix = ConnectFileBackedMatrix<double>(_fileName, filePath,
          _dataRegionPtrs, _nrow, _ncol);
    }
  }
  CreateMutexes(_mutexPtrs, _sharedName, _ncol);
  _mutexLock.init( _sharedName+"_mutex_lock" );
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

void DestroyFileBackedSepMatrix( const std::string &sharedName, 
  const unsigned long ncol, const std::string &fileName, const bool preserve )
{
  unsigned long i;
  for (i=0; i < ncol; ++i)
  {
    shared_memory_object::remove((sharedName + "_column_" + ttos(i)).c_str());
    if (!preserve)
    {
      std::string removeFileName(fileName + "_column_" + ttos(i));
      unlink( removeFileName.c_str() );
    }
  }
}

bool FileBackedBigMatrix::destroy()
{
  named_mutex mutex( open_or_create, (_sharedName+"_counter_mutex").c_str() );
  mutex.lock();
  _dataRegionPtrs.resize(0);
  if (_sepCols)
  {
    if (_sharedCounter.get() == 1)
    {
      DestroyFileBackedSepMatrix(_sharedName, _ncol, _fileName, _preserve);
    }
    if (_matrix)
    {
      switch(_matType)
      {
        case 1:
          delete [] reinterpret_cast<char**>(_matrix);
          break;
        case 2:
          delete [] reinterpret_cast<short**>(_matrix);
          break;
        case 4:
          delete [] reinterpret_cast<int**>(_matrix);
          break;
        case 8:
          delete [] reinterpret_cast<double**>(_matrix);
      }
    }
  }
  else
  {
    if (_sharedCounter.get() == 1)
    {
      shared_memory_object::remove(_sharedName.c_str());
    	if (!_preserve)
    	{
      	unlink( _fileName.c_str() );
    	}
    }
  }
  if (_sharedCounter.get() == 1)
  {
    long i;
    for (i=0; i < static_cast<long>(_mutexPtrs.size()); ++i)
    {
      _mutexPtrs[i]->destroy();
    }
    _mutexLock.destroy();
  }
  mutex.unlock();
  named_mutex::remove((_sharedName+"_counter_mutex").c_str());
  return true;
}

// Add and remove columns
/*
template<typename T>
void RemAndCopy(BigMatrix &bigMat, long remCol, long newNumCol)
{
  T** oldMat = reinterpret_cast<T**>(bigMat.matrix());
  T** newMat = new T*[newNumCol];
  delete [] (oldMat[remCol]);
  long i,j;
  for (i=0,j=0; i < newNumCol+1; ++i)
  {
    if (i != remCol)
      newMat[j++] = oldMat[i];
  }
  delete [] oldMat;
  bigMat.matrix() = reinterpret_cast<void*>(newMat);
}

bool BigMatrix::remove_column(long col)
{
  if (!_pColNames->empty())
    _pColNames->erase(_pColNames->begin()+col);
  --_ncol;
  switch (_matType)
  {
    case 1:
      RemAndCopy<char>(*this, col, _ncol);
      break;
    case 2:
      RemAndCopy<short>(*this, col, _ncol);
      break;
    case 4:
      RemAndCopy<int>(*this, col, _ncol);
      break;
    case 8:
      RemAndCopy<double>(*this, col, _ncol);
      break;
  }
  return true;
}

template<typename T>
void AddAndCopy(BigMatrix &bigMat, long pos, long newNumCol, long nrow, 
  double init)
{
  T** oldMat = reinterpret_cast<T**>(bigMat.matrix());
  T* addRow = new T[nrow];
  T** newMat = new T*[newNumCol];
  long i,j;

  for (i=0; i < nrow; ++i)
    addRow[i] = (T)init;

  bool added=false;
  for (i=0,j=0; i < newNumCol-1; ++i)
  {
    if (j==pos)
    {
      newMat[j++] = addRow;
      added=true;
    }
    newMat[j++] = oldMat[i];
  }
  if (!added)
    newMat[j] = addRow;
  delete [] reinterpret_cast<T**>(bigMat.matrix());
  bigMat.matrix() = reinterpret_cast<void*>(newMat);
}

bool BigMatrix::insert_column(long pos, double init, string name)
{
  if (!_pColNames->empty())
    _pColNames->insert( _pColNames->begin()+pos, name);
  ++_ncol;
  switch (_matType)
  {
    case 1:
      AddAndCopy<char>(*this, pos, _ncol, _nrow, init);
      break;
    case 2:
      AddAndCopy<short>(*this, pos, _ncol, _nrow, init);
      break;
    case 4:
      AddAndCopy<int>(*this, pos, _ncol, _nrow, init);
      break;
    case 8:
      AddAndCopy<double>(*this, pos, _ncol, _nrow, init);
      break;
  }
  return true;
}
*/
