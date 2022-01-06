// cpptest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <vector>
#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <optional>

class stoppedException :public std::exception
{

};

struct ReadWriteCondition
{

	ReadWriteCondition(std::mutex& readMutex, std::condition_variable& readcnd, std::mutex& writeMutex, std::condition_variable& writecnd) :
		read_mutex_(readMutex), readcnd_(readcnd), write_mutex_(writeMutex), writecnd_(writecnd)
	{
	}

	std::mutex& read_mutex_;
	std::condition_variable& readcnd_;
	std::mutex& write_mutex_;
	std::condition_variable& writecnd_;
};

template <typename T, int SIZE>
class Buffer;
template <typename T, int SIZE, bool IsREADER>
class BufferHolder;

template <typename T, int SIZE>
class BufferHolder<T, SIZE, false>
{

public:
	BufferHolder(Buffer<T, SIZE>& buffer, int index)
		:buffer_{ buffer }, index_{ index }
	{

	}
	BufferHolder(BufferHolder<T, SIZE, false>&& other) noexcept
		:buffer_{ other.buffer_ }, index_{ other.index_ }
	{
		other.index_ = -1;
	}

	BufferHolder(const BufferHolder<T, SIZE, false>&) = delete;
	BufferHolder<T, SIZE, false> operator =(const BufferHolder<T, SIZE, false>&) = delete;
	BufferHolder<T, SIZE, false> operator =(BufferHolder<T, SIZE, false>&&) = delete;
	~BufferHolder()
	{
		if (index_ != -1)
			buffer_.SetWriteComplete();

	}
	std::vector<T>& operator*()
	{
		return buffer_.data_;
	}
	int GetIndex() const
	{
		return index_;
	}

private:
	Buffer<T, SIZE>& buffer_;
	const int index_;

};

template <typename T, int SIZE>
class BufferHolder<T, SIZE, true>
{

public:

	BufferHolder(Buffer<T, SIZE>& buffer, int index)
		:buffer_{ buffer }, index_{ index }
	{

	}

	BufferHolder(BufferHolder<T, SIZE, true>&& other) noexcept
		:buffer_{ other.buffer_ }, index_{ other.index_ }
	{
		other.index_ = -1;
	}

	BufferHolder(const BufferHolder<T, SIZE, true>&) = delete;
	BufferHolder<T, SIZE, true> operator =(const BufferHolder<T, SIZE, true>&) = delete;
	BufferHolder<T, SIZE, true> operator =(BufferHolder<T, SIZE, true>&&) = delete;

	~BufferHolder()
	{
		if (index_ != -1)
			buffer_.SetReadComplete();
	}
	const std::vector<T>& operator*()
	{
		return buffer_.data_;
	}
	int GetIndex() const
	{
		return index_;
	}

private:
	Buffer<T, SIZE>& buffer_;
	int index_;

};



template <typename T, int SIZE>
class Buffer
{
	friend class BufferHolder<T, SIZE, false>;
	friend class BufferHolder<T, SIZE, true>;
	enum class BufferState
	{
		Reading, Writing, Readed, Writed
	};
public:
	Buffer()
	{
		data_.resize(SIZE);
		state_ = BufferState::Readed;
	}

	

	bool IsReadyForWrite()
	{
		std::lock_guard<std::mutex> lock{ lock_ };
		if (state_ == BufferState::Readed)
		{
			state_ = BufferState::Writing;
			return true;
		}
		return false;


	}
	bool IsReadyForRead()
	{
		std::lock_guard<std::mutex> lock{ lock_ };
		if (state_ == BufferState::Writed)
		{
			state_ = BufferState::Reading;
			return true;
		}
		return false;

	}
	void SetReadComplete()
	{
		std::lock_guard<std::mutex> lock{ lock_ };
		state_ = BufferState::Readed;
		//notify
		std::lock_guard<std::mutex> rwcndLock{ rwcnd_->write_mutex_ };
		rwcnd_->writecnd_.notify_one();

	}
	void SetWriteComplete()
	{
		std::lock_guard<std::mutex> lock{ lock_ };
		state_ = BufferState::Writed;
		//notify
		std::lock_guard<std::mutex> rwcndLock{ rwcnd_->read_mutex_ };
		rwcnd_->readcnd_.notify_one();
	}

	void SetReadWriteCondition(std::shared_ptr<ReadWriteCondition> rcwnd)
	{
		rwcnd_ = std::move(rcwnd);
	}
private:
	std::mutex lock_;
	std::vector<T> data_;
	BufferState state_;
	std::shared_ptr<ReadWriteCondition> rwcnd_;
};





template <typename T, int SIZE, int COUNT>
class MultipleBuffer
{
public:
	MultipleBuffer()
	{
		readWriteCondition_ = std::make_shared<ReadWriteCondition>(waitForReadLock_, waitForRead_, waitForWriteLock_, waitForWrite_);
		for (auto& buffer : buffers_)
		{
			buffer.SetReadWriteCondition(readWriteCondition_);
		}
	}
	BufferHolder<T, SIZE, false> GetForWrite()
	{

		const auto writeIndex = GetWriteIndex();
		if (writeIndex == -1)
		{
			throw stoppedException{};
		}
		assert(writeIndex < COUNT);
		assert(writeIndex >= 0);
		return BufferHolder<T, SIZE, false>{buffers_[writeIndex], writeIndex};

	}
	BufferHolder<T, SIZE, true> GetForRead()
	{
		const auto readIndex = GetReadIndex();
		if (readIndex == -1)
		{
			throw stoppedException{};
		}
		assert(readIndex < COUNT);
		assert(readIndex >= 0);
		return BufferHolder<T, SIZE, true>{buffers_[readIndex], readIndex};
	}

	void Stop()
	{
		isStopped_ = true;
		std::lock_guard<std::mutex> writeLock{ waitForWriteLock_ };
		waitForWrite_.notify_all();
		std::lock_guard<std::mutex> readLock{ waitForReadLock_ };
		waitForRead_.notify_all();
	}

private:
	int GetWriteIndex()
	{
		bool get = false;
		int index = 0;
		while (!get && !isStopped_)
		{
			index = 0;
			for (auto& buffer : buffers_)
			{
				if (buffer.IsReadyForWrite())
				{
					get = true;
					break;
				}
				index++;
			}
			if (!get)
			{
				std::unique_lock<std::mutex> waitLock{ waitForWriteLock_ };
				waitForWrite_.wait(waitLock);
			}

		}
		if (!get)
		{
			return -1;
		}
		return index;
	}
	int GetReadIndex()
	{
		int index = 0;
		bool get = false;
		while (!get && !isStopped_)
		{
			index = 0;
			for (auto& buffer : buffers_)
			{
				if (buffer.IsReadyForRead())
				{
					get = true;
					break;
				}
				index++;
			}

			if (!get)
			{
				std::unique_lock<std::mutex> waitLock{ waitForReadLock_ };
				waitForRead_.wait(waitLock);
			}
		}
		if (!get)
		{
			return -1;
		}
		return index;
	}

	Buffer<T, SIZE> buffers_[COUNT];
	std::shared_ptr<ReadWriteCondition> readWriteCondition_;
	std::condition_variable waitForWrite_;
	std::condition_variable waitForRead_;
	std::mutex waitForWriteLock_;
	std::mutex waitForReadLock_;
	bool isStopped_ = false;

};
using FrameMultipleBuffer = MultipleBuffer<unsigned char, 500, 5>;
void Writer(FrameMultipleBuffer& dblBuffer, std::atomic_bool& isStopped)
{
	while (!isStopped)
	{
		try
		{
			BufferHolder<unsigned char, 500, false> bufferForWrite{ dblBuffer.GetForWrite() };
			(*bufferForWrite).push_back(52);
			std::cout << "Get Buffer For Write: " << bufferForWrite.GetIndex() << "\n";
			//for testing slow writer
			//std::this_thread::sleep_for(std::chrono::milliseconds(1800));
		}
		catch (const stoppedException&)
		{
			return;
		}

	}
}
void Reader(FrameMultipleBuffer& dblBuffer, std::atomic_bool& isStopped)
{
	while (!isStopped)
	{
		try
		{
			BufferHolder<unsigned char, 500, true> bufferForRead{ dblBuffer.GetForRead() };
			[[maybe_unused]]auto data=(*bufferForRead).at(0);
			std::cout << "Get Buffer For Read: " << bufferForRead.GetIndex() << "\n";
			//for testing slow reader
			 std::this_thread::sleep_for(std::chrono::milliseconds(1800));
			
		}
		catch (const stoppedException&)
		{
			return;
		}

	}
}

int  main(void)
{

	std::atomic_bool isStopped = false;
	const int writerCount = 2;
	const int readerCount = 3;
	FrameMultipleBuffer dblBuffer;
	std::vector<std::thread> writers;
	std::vector<std::thread> readers;
	for (int i = 0; i < writerCount; i++)
	{
		std::thread wThread{ Writer,std::ref(dblBuffer),std::ref(isStopped) };
		writers.push_back(std::move(wThread));
	}
	for (int i = 0; i < readerCount; i++)
	{
		std::thread rThread{ Reader,std::ref(dblBuffer),std::ref(isStopped) };
		readers.push_back(std::move(rThread));
	}

	system("pause");
	isStopped.store(true);
	dblBuffer.Stop();
	for (auto& wThread : writers)
	{
		wThread.join();
	}
	for (auto& rThread : readers)
	{
		rThread.join();
	}
	std::cout << "finished\n";
	system("pause");
	return 0;
}
