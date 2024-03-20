#include "runner.h"
#define TIMEOUT 50
#include <signal.h>


Runner::Runner(ArgumentParser argParser){
    this->ip_address = argParser.server_ip;
    this->port = std::to_string(argParser.port);
    this->protocol = argParser.protocol;
    this->timeout = argParser.timeout;
    this->retries = argParser.retries;
    if(this->protocol == Connection::Protocol::UDP){
        this->client = UDPClient(this->ip_address,this->port);
    }
    else{
        //TODO: Implement TCPClient in runner
        // this->client = TCPClient(this->ip_address,this->port);
    }
}

void handle_sigint(int sig) {
    ByePacket byePacket;

    std::string serialized_byePacket = byePacket.serialize();

    // client.send(serialized_byePacket);

    exit(0);
}



void Runner::inputScanner(Connection &connection){
    std::string line;
    while (std::getline(std::cin, line)) {
        Input userInput;
        userInput.getNewInput(line);
        
        std::lock_guard<std::mutex> lock(queue_mutex);
    
        input_packet_queue.push(userInput.parseInput(connection));
        //if userInput is nullpacket, then continue
        if(std::holds_alternative<NullPacket>(input_packet_queue.back())){
            std::cout << "Invalid input" << std::endl;
            input_packet_queue.pop();
            continue;
        }    
        connection.message_id_map[connection.message_id-1] = false;

        queue_cond_var.notify_one(); 
    }

}

void Runner::packetSender(Connection &connection) {
        while(1) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cond_var.wait(lock, [&] { return !input_packet_queue.empty(); });
            if(input_packet_queue.empty()){
                continue;
            }

            //get the first packet from the queue and call .serialize
            send_packet = input_packet_queue.front();

            std::string serialized_packet = "";
            std::visit([&](auto& p) { serialized_packet = p.serialize(connection); }, send_packet);

            input_packet_queue.pop();
            lock.unlock();
            
        for (int attempt = 0; attempt < this->retries+1; ++attempt) {
                client.send(serialized_packet);

                std::cout << "Waiting\n";

                std::unique_lock<std::mutex> reply_lock(reply_mutex);
                if(reply_cond_var.wait_for(reply_lock, std::chrono::milliseconds(this->timeout)) == std::cv_status::timeout) {
                    std::cout << "Timeout\n";
                } else {
                    std::cout << "Good\n";
                    std::cout << "Message sent: " << serialized_packet << std::endl; // Print the message that was sent
                    break; // Break out of the loop if the message was sent successfully
                }
            }
        }
    }


void Runner::packetReceiver(Connection &connection) {
    while (1) {
        std::string reply = client.receive();

        std::variant<RECV_PACKET_TYPE> recv_packet = ReceiveParser(reply, connection);
        
        if (std::holds_alternative<AuthPacket>(send_packet) || std::holds_alternative<JoinPacket>(send_packet)) {
            processAuthJoin(connection, reply, recv_packet);
        }
        else if(std::holds_alternative<ConfirmPacket>(recv_packet)){
            ConfirmPacket confirm_packet = std::get<ConfirmPacket>(recv_packet);
            std::vector<std::string> packet_data = confirm_packet.getData();
            uint16_t messageID = std::stoi(packet_data[0]);
            if(connection.message_id_map[messageID] == false){
                connection.message_id_map[messageID] = true;
            }         
            else{
                std::cout << "incorrect id";
                continue;
            }   
        }
        else{
            //call the .getData() method of the packet and print the data
            std::cout << "Message received: \n";
            std::vector<std::string> packet_data = std::visit([&](auto& p) { return p.getData(); }, recv_packet);
            std::cout << packet_data[1] << std::endl;
            uint16_t messageID = std::stoi(packet_data[2]);
            ConfirmPacket confirm_packet(messageID);
            client.send(confirm_packet.serialize());
        }
        
        std::unique_lock<std::mutex> lock(reply_mutex);
        reply_cond_var.notify_one();
    }
}

void Runner::processAuthJoin(Connection &connection, std::string &reply, std::variant<RECV_PACKET_TYPE> recv_packet) {
    if (std::holds_alternative<ReplyPacket>(recv_packet)) {
        handleReplyPacket(connection, reply, recv_packet);
    }
    else if (std::holds_alternative<ConfirmPacket>(recv_packet)) {
        handleConfirmPacket(connection, reply, recv_packet);
    }
}

void Runner::handleReplyPacket(Connection &connection, std::string &reply, std::variant<RECV_PACKET_TYPE> recv_packet) {
    ReplyPacket reply_packet = std::get<ReplyPacket>(recv_packet);
    reply = client.receive();
    std::vector<std::string> packet_data = reply_packet.getData();

    if (packet_data[0] != "1") {
        connection.clearAfterAuth();
        return; // No further processing needed if authentication failed
    }
    std::cout << "Message: " << packet_data[1] << std::endl;    

    std::variant<RECV_PACKET_TYPE> next_recv_packet = ReceiveParser(reply, connection);
    if (std::holds_alternative<ConfirmPacket>(next_recv_packet)) {
        ConfirmPacket confirm_packet = std::get<ConfirmPacket>(next_recv_packet);
        client.send(reply);
    }
}

void Runner::handleConfirmPacket(Connection &connection, std::string &reply, std::variant<RECV_PACKET_TYPE> recv_packet) {
    ConfirmPacket confirm_packet = std::get<ConfirmPacket>(recv_packet);
    std::string prev_reply = reply;
    reply = client.receive();
    
    std::variant<RECV_PACKET_TYPE> next_recv_packet = ReceiveParser(reply, connection);
    if (std::holds_alternative<ReplyPacket>(next_recv_packet)) {
        ReplyPacket reply_packet = std::get<ReplyPacket>(next_recv_packet);
        std::vector<std::string> packet_data = reply_packet.getData();
        if (packet_data[0] != "1") {
            connection.clearAfterAuth();
            return; // No need to proceed if authentication failed
        }
        std::cout << "Message: " << packet_data[1] << std::endl;    

        client.send(prev_reply);
    }
}


void Runner::run(){
    Connection connection = Connection(ip_address, port, Connection::Protocol::UDP);

    std::cout << "Connected \n";
    
    // signal(SIGINT, handle_sigint);

     //thread for reading stdin, parsing them and sending them to other thread
     std::jthread inputThread([&](){
        inputScanner(connection);
    });

    //thread for sending messages to server
    std::jthread sendThread([&]() {
        //while first thread is running, this thread will be waiting for input
        packetSender(connection);
    });
    
    std::jthread receiveThread([&]() {
        packetReceiver(connection);
    });
}