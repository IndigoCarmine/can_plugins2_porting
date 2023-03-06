#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind/bind.hpp>

#include <chrono>
#include <future>
#include <vector>

#include "cobs.hpp"
#include "test.hpp"
#include <can_plugins2_porting/Frame.h>

using namespace std::chrono_literals;
using namespace std::placeholders;


namespace slcan_command
{
    enum Command:uint8_t{
        Normal=0,
        Negotiation=1,

    };
} // namespace slcan_command


namespace can_plugins2_porting
{
    class SlcanBridge: public  nodelet::Nodelet
    {
        private:

            /////////////Slacan Status///////////////////
            //serial port connected but "HelloSlcan" has not been returned yet.
            bool is_connected_ = false;
            //sconection with usbcan is active. the serial port is new usbcan.
            bool is_active_ = false;
            /////////////Slacan Status///////////////////



            std::shared_ptr<boost::asio::io_context> io_context_;
            std::shared_ptr<boost::asio::serial_port> serial_port_;
            //it will prohabit the io_context to stop. 
            std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;

            boost::asio::streambuf read_streambuf_;

            //thread fot running io_context
            std::thread io_context_thread_; 

            std::unique_ptr<std::thread> reading_thread_;

            ros::Publisher can_rx_pub_;
            ros::Subscriber can_tx_sub_;

            void canRxCallback(const can_plugins2_porting::Frame::ConstPtr& msg);

            const int initialize_timeout_ = 1000;//ms
            //port open and setting.
            bool initializeSerialPort(const std::string port_name);


            const int handshake_timeout_ = 1000;//ms
            //check the serial deveice is usbcan.
            bool handshake();


            // convert message from usbcan, and process it.
            void readingProcess(const std::vector<uint8_t> data);

            void asyncWrite(const can_plugins2_porting::Frame::ConstPtr& msg);
            void asyncWrite(const slcan_command::Command command,const std::vector<uint8_t> data);

             //Directly use is deprecated.
            void asyncWrite(const std::vector<uint8_t> data);

            //you should call this function at once after the connection is established.
            void asyncRead();

            void asyncReadOnce();

            //these function will be called when the data is read from the serial port.
            void readOnceHandler(const boost::system::error_code& error, std::size_t bytes_transferred);
            void readHandler(const boost::system::error_code& error, std::size_t bytes_transferred);

            //these function will be called when the data is written to the serial port. for error handling.
            void writeHandler(const boost::system::error_code& error, std::size_t bytes_transferred);

            public:
            void onInit() override{
                ros::NodeHandle& nh = getNodeHandle();
                can_rx_pub_ = nh.advertise<can_plugins2_porting::Frame>("can_rx", 10);
                can_tx_sub_ = nh.subscribe<can_plugins2_porting::Frame>("can_tx", 10, &SlcanBridge::canRxCallback, this);

                std::string port_name="/dev/usbcan2";

                //initialize asio members.
                io_context_ = std::make_shared<boost::asio::io_context>();
                serial_port_ = std::make_shared<boost::asio::serial_port>(io_context_->get_executor());
                work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(io_context_->get_executor());

                //start io_context thread.
                io_context_thread_ = std::thread([this](){
                    io_context_->run();
                });

                NODELET_INFO("SlcanBridge is initialized.");

                bool i = initializeSerialPort(port_name);
                if(i){
                    NODELET_INFO("SlcanBridge isconnected.");
                    asyncRead();
                    handshake();
                }else{
                    NODELET_ERROR("SlcanBridge is not connected.");
                }
        
            }

            boost::array<unsigned char, 32> receive_api_frame_;

            void onShutdown(){
                //it generates copilot. so, it may have some problems. TODO:::CHECK
                is_active_ = false;
                io_context_->stop();
                io_context_thread_.join();
                serial_port_->close();   
            }

            ~SlcanBridge(){
                onShutdown();
            }
    };   

    void SlcanBridge::canRxCallback(const can_plugins2_porting::Frame::ConstPtr& msg){
        if(!is_active_) return;
        asyncWrite(msg);
    }

        //port open and setting. 
    bool SlcanBridge::initializeSerialPort(const std::string port_name){
        ros::Rate rate = ros::Rate(1);
        while(true){
            try{
                serial_port_->open(port_name);
                serial_port_->set_option(boost::asio::serial_port_base::character_size(8));
                serial_port_->set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));
                serial_port_->set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
                serial_port_->set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
                serial_port_->set_option(boost::asio::serial_port_base::baud_rate(115200));
            }catch(boost::system::system_error e){
                switch (e.code().value()){
                    case 2:
                        NODELET_ERROR("Cannot connect. No such a device");
                        break;
                    case 13:
                        NODELET_ERROR("Cannot connect. Permission denied");
                        break;
                    case 25:
                        NODELET_ERROR("Cannot connect. Inappropriate ioctl for device. There is  mistakes in udev rule, I think.");
                        break;
                    default:
                        NODELET_ERROR("Cannot connect. Unknown error");
                        break;
                }
            }
            if(serial_port_->is_open()){
                NODELET_INFO("Serial port is opened."); 
                is_connected_ = true;
                break;
            }
            rate.sleep();

        }
        return true;
    }


    void SlcanBridge::asyncWrite(const std::vector<uint8_t> data){
        io_context_->post([this,data](){
            boost::asio::async_write(*serial_port_,boost::asio::buffer(data),
            boost::bind(&SlcanBridge::writeHandler,this,boost::asio::placeholders::error,boost::asio::placeholders::bytes_transferred));
        });
        return;
    }

    void SlcanBridge::asyncWrite(const can_plugins2_porting::Frame::ConstPtr &msg){
        // data structure
        /*
        uint8_t command & frame_type: (command: if it is normal can frame, it is 0x00.)<<4 | is_rtr << 2 | is_extended << 1 | is_error
        uint8_t id[4] : can id
        uint8_t dlc : data length
        uint8_t data[8] : data
        */
        std::vector<uint8_t> data(6+8);
        data[0] = (msg->is_rtr << 2) | (msg->is_extended << 1) | (msg->is_error);
        data[6] = msg->dlc;
        data[1] = (msg->id >> 24) & 0xff;
        data[2] = (msg->id >> 16) & 0xff;
        data[3] = (msg->id >> 8) & 0xff;
        data[4] = msg->id & 0xff;
        for(int i = 0; i < 8; i++){
            data[6+i] = msg->data[i];
        }
        
        std::vector<uint8_t> output = cobs::encode(data);
         
        asyncWrite(output);
    }

    void SlcanBridge::asyncWrite(const slcan_command::Command command,const std::vector<uint8_t> data){
        if(command == slcan_command::Normal)
            NODELET_ERROR("asyncWrite(Command) can not use normal. you need to use asyncWrite(Frame)");

        // data structure
        /*
        uint8_t command & frame_type: (command: if it is normal can frame, it is 0x00.)<<4 | is_rtr << 2 | is_extended << 1 | is_error
        uint8_t id[4] : data
        */
        std::vector<uint8_t> raw_data(1+data.size());
        raw_data[0] = (command << 4);
        for(std::size_t i = 0; i < data.size(); i++){
            raw_data[1+i] = data[i];
        }
        std::vector<uint8_t> output = cobs::encode(raw_data);
        
        asyncWrite(output);
    }


    void SlcanBridge::readingProcess(const std::vector<uint8_t> data){
        std::vector<uint8_t> cobs_output_buffer_ = cobs::decode(data);

        NODELET_INFO("RreadingProcess %s",test::hex_to_string(cobs_output_buffer_ ).c_str());
        NODELET_INFO("Text length %ld",cobs_output_buffer_ .size());
        //check it is handshake. USBCAN will send "HelloSlcan" when the connection is established.
        if(cobs_output_buffer_.size() == 10+1){
            static const uint8_t HelloSlcan[] ={0x01<<4,'H','e','l','l','o','S','L','C','A','N'};
            bool is_handshake = true;
            for(int i = 0; i < 12; i++){
                if(cobs_output_buffer_[i] != HelloSlcan[i]){
                    is_handshake = false;
                    break;
                }
            }
            if(is_handshake){
                NODELET_INFO("handshake");
                is_active_ = true;
                return;
            }
        }





        //publish the data to the topic.
        if(data.size()<12){
            NODELET_ERROR("data size is too small");
            return;
        }

        // data structure
        /*
        uint8_t command & frame_type: (command: if it is normal can frame, it is 0x00.)<<4 | is_rtr << 2 | is_extended << 1 | is_error
        uint8_t id[4] : can id
        uint8_t dlc : data length
        uint8_t data[8] : data
        */
        can_plugins2_porting::Frame msg;
        msg.is_error = cobs_output_buffer_[0]&0x1;
        msg.is_extended = cobs_output_buffer_[0]>>1&0x1;
        msg.is_rtr = cobs_output_buffer_[0]>>2&0x1;
        msg.id = cobs_output_buffer_[1]<<12|cobs_output_buffer_[2]<<8|cobs_output_buffer_[3]<<4|cobs_output_buffer_[4];
        msg.dlc = cobs_output_buffer_[5];
        for(int i = 0; i < 8; i++){
            msg.data[i] = cobs_output_buffer_[4+i];
        }
        can_rx_pub_.publish(msg);
        return;
    }



    bool SlcanBridge::handshake(){
        ros::Rate rate(1);
        while(!is_active_){
            const std::vector<uint8_t> HelloUSBCAN = {'H','e','l','l','o','U','S','B','C','A','N'};
            asyncWrite(slcan_command::Negotiation,HelloUSBCAN);
            NODELET_INFO("Waitting for negotiation...");
            rate.sleep();
        }
        return true;
    }


    void SlcanBridge::readOnceHandler(const boost::system::error_code& error, std::size_t bytes_transferred){
        if(error){
            NODELET_ERROR("readOnceHandler error");
            return;
        }

        std::vector<uint8_t> data(bytes_transferred);

        //it can use iostream but
        uint8_t* data_ptr = (uint8_t*)boost::asio::buffer_cast<const char*>(read_streambuf_.data());
        for(std::size_t i = 0; i < bytes_transferred; i++){
            data[i] = data_ptr[i];
        }


        SlcanBridge::readingProcess(data);
        
        // RCLCPP_INFO(get_logger(),"readOnceHandler %s",test::hex_to_string(data,bytes_transferred).c_str());
        read_streambuf_.consume(bytes_transferred);
        return;
    }

    void SlcanBridge::readHandler(const boost::system::error_code& error, std::size_t bytes_transferred){
        readOnceHandler(error,bytes_transferred);
        asyncRead();
        return;
    }

    //write data to the serial port. it calls asyncReadOnce() after reading.
    void SlcanBridge::asyncReadOnce(){
        //read and write functions can worl in the same time.
        //so, it is not necessary to use io_context_->post()      (this is a strand.)
        boost::asio::async_read_until(*serial_port_,read_streambuf_,'\0',
            boost::bind(&SlcanBridge::readOnceHandler,this,boost::asio::placeholders::error,boost::asio::placeholders::bytes_transferred));

        return;
    }

    void SlcanBridge::writeHandler(const boost::system::error_code& error, std::size_t bytes_transferred){
        if(error){
            NODELET_ERROR("writeHandler error: tried to write %ld byte",bytes_transferred);
            
            //the followings are generated by copilot.
            //TODO:CHECK IT!
            switch (error.value())
            {
            case boost::system::errc::no_such_device_or_address:
                NODELET_ERROR("no_such_device_or_address");
                break;
            case boost::system::errc::no_such_file_or_directory:
                NODELET_ERROR("no_such_file_or_directory");
                break;
            case boost::system::errc::permission_denied:
                NODELET_ERROR("permission_denied");
                break;
            case boost::system::errc::bad_file_descriptor:
                NODELET_ERROR("bad_file_descriptor");
                break;
            case boost::system::errc::resource_unavailable_try_again:
                NODELET_ERROR("resource_unavailable_try_again");
                break;
            default:
                NODELET_ERROR("unknown error");
                break;
            }
        }
    }

    //write data to the serial port. it calls asyncRead() after reading.
    void SlcanBridge::asyncRead(){
        //read and write functions can worl in the same time.
        //so, it is not necessary to use io_context_->post()      (this is a strand.)
        boost::asio::async_read_until(*serial_port_,read_streambuf_,'\0',
            boost::bind(&SlcanBridge::readHandler,this,boost::asio::placeholders::error,boost::asio::placeholders::bytes_transferred));
        return;
    }

} // namespace can_plugins2_porting
PLUGINLIB_EXPORT_CLASS(can_plugins2_porting::SlcanBridge, nodelet::Nodelet);

