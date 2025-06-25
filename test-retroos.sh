#!/bin/bash
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🧪 RetroOS Application Testing Script${NC}"
echo -e "${BLUE}===================================${NC}"

# Test configuration
BACKEND_URL="http://localhost:8001"
FRONTEND_URL="http://localhost:3000"
API_BASE="$BACKEND_URL/api"

# Test functions
test_service() {
    local service_name=$1
    local url=$2
    local expected_status=${3:-200}
    
    echo -n -e "${YELLOW}Testing $service_name... ${NC}"
    
    if response=$(curl -s -w "%{http_code}" "$url" -o /tmp/response); then
        status_code="${response: -3}"
        if [[ "$status_code" == "$expected_status" ]]; then
            echo -e "${GREEN}✅ PASS${NC}"
            return 0
        else
            echo -e "${RED}❌ FAIL (Status: $status_code)${NC}"
            return 1
        fi
    else
        echo -e "${RED}❌ FAIL (Connection error)${NC}"
        return 1
    fi
}

test_api_endpoint() {
    local endpoint=$1
    local method=${2:-GET}
    local expected_status=${3:-200}
    local data=${4:-""}
    
    local url="$API_BASE$endpoint"
    
    echo -n -e "${YELLOW}Testing API $method $endpoint... ${NC}"
    
    if [[ "$method" == "POST" && -n "$data" ]]; then
        response=$(curl -s -w "%{http_code}" -X POST -H "Content-Type: application/json" -d "$data" "$url" -o /tmp/response)
    else
        response=$(curl -s -w "%{http_code}" -X "$method" "$url" -o /tmp/response)
    fi
    
    status_code="${response: -3}"
    if [[ "$status_code" == "$expected_status" ]]; then
        echo -e "${GREEN}✅ PASS${NC}"
        return 0
    else
        echo -e "${RED}❌ FAIL (Status: $status_code)${NC}"
        if [[ -f /tmp/response ]]; then
            echo -e "${RED}Response: $(cat /tmp/response)${NC}"
        fi
        return 1
    fi
}

# Main testing flow
main() {
    local failed_tests=0
    local total_tests=0
    
    echo -e "${BLUE}🔍 Testing Basic Connectivity${NC}"
    echo -e "${BLUE}=============================${NC}"
    
    # Test basic service connectivity
    ((total_tests++))
    test_service "Backend Health" "$BACKEND_URL/api/" || ((failed_tests++))
    
    ((total_tests++))
    test_service "Frontend" "$FRONTEND_URL" || ((failed_tests++))
    
    echo -e ""
    echo -e "${BLUE}🔍 Testing Backend API Endpoints${NC}"
    echo -e "${BLUE}=================================${NC}"
    
    # Test all API endpoints
    ((total_tests++))
    test_api_endpoint "/" || ((failed_tests++))
    
    ((total_tests++))
    test_api_endpoint "/wallpapers" || ((failed_tests++))
    
    ((total_tests++))
    test_api_endpoint "/system-info" || ((failed_tests++))
    
    # Test user registration
    ((total_tests++))
    test_api_endpoint "/register" "POST" 200 '{"username":"testuser123","password":"testpass123"}' || ((failed_tests++))
    
    # Test user login
    ((total_tests++))
    test_api_endpoint "/login" "POST" 200 '{"username":"testuser123","password":"testpass123"}' || ((failed_tests++))
    
    echo -e ""
    echo -e "${BLUE}📊 Test Results${NC}"
    echo -e "${BLUE}===============${NC}"
    
    local passed_tests=$((total_tests - failed_tests))
    echo -e "Total Tests: $total_tests"
    echo -e "${GREEN}Passed: $passed_tests${NC}"
    echo -e "${RED}Failed: $failed_tests${NC}"
    
    if [[ $failed_tests -eq 0 ]]; then
        echo -e ""
        echo -e "${GREEN}🎉 All tests passed! RetroOS is working correctly.${NC}"
        echo -e "${GREEN}🌐 Access the application at: $FRONTEND_URL${NC}"
        return 0
    else
        echo -e ""
        echo -e "${RED}❌ Some tests failed. Please check the issues above.${NC}"
        return 1
    fi
}

# Error handling
trap 'echo -e "${RED}❌ Test script interrupted${NC}"; exit 1' INT

# Run main function
main

# Cleanup
rm -f /tmp/response